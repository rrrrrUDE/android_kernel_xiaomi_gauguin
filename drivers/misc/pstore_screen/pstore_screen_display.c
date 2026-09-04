// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pstore_screen.h>
#include <linux/refcount.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include "pstore_screen_display_internal.h"

#define PS_PROVIDER_STATE_PUBLISHED 1
#define PS_PROVIDER_STATE_CLAIMED 2
#define PS_PROVIDER_STATE_RETIRED 3

struct pstore_screen_display_handle {
	char name[PSTORE_SCREEN_DISPLAY_NAME_LEN];
	struct module *owner;
	struct pstore_screen_scanout_desc public_scanout;
	struct pstore_screen_display_ops public_ops;
	void *public_ctx;
	struct ps_scanout scanout;
	struct ps_layout layout;
	struct ps_display_candidate candidate;
	refcount_t refs;
	atomic_t state;
	u32 slot;
	u32 active;
};

static_assert(sizeof(struct pstore_screen_display_desc) ==
	      PSTORE_SCREEN_DISPLAY_DESC_V1_SIZE,
	      "pstore screen display v1 layout must remain frozen");

static DEFINE_MUTEX(ps_display_registry_lock);
static atomic64_t ps_display_generation = ATOMIC64_INIT(0);
static struct pstore_screen_display_handle __rcu
	*ps_display_registry[PSTORE_SCREEN_DISPLAY_MAX_PROVIDERS];

static int ps_provider_write_span(void *ctx, u64 offset, const u8 *source,
				  u32 length)
{
	struct pstore_screen_display_handle *provider = ctx;

	return provider->public_scanout.write_span(
		provider->public_scanout.ctx, offset, source, length);
}

static int ps_provider_flush(void *ctx)
{
	struct pstore_screen_display_handle *provider = ctx;

	return provider->public_scanout.flush(provider->public_scanout.ctx);
}

static int ps_provider_reserve(void *ctx, u64 generation, u64 *token)
{
	struct pstore_screen_display_handle *provider = ctx;
	int ret;

	if (atomic_cmpxchg(&provider->state, PS_PROVIDER_STATE_PUBLISHED,
			   PS_PROVIDER_STATE_CLAIMED) !=
	    PS_PROVIDER_STATE_PUBLISHED)
		return -ENODEV;

	if (!provider->public_ops.reserve) {
		*token = 0U;
		return 0;
	}
	ret = provider->public_ops.reserve(provider->public_ctx, generation,
					   token);
	if (ret) {
		if (atomic_cmpxchg(&provider->state, PS_PROVIDER_STATE_CLAIMED,
				   READ_ONCE(provider->active) ?
				   PS_PROVIDER_STATE_PUBLISHED :
				   PS_PROVIDER_STATE_RETIRED) ==
		    PS_PROVIDER_STATE_CLAIMED &&
		    !READ_ONCE(provider->active))
			atomic_cmpxchg(&provider->state,
				       PS_PROVIDER_STATE_PUBLISHED,
				       PS_PROVIDER_STATE_RETIRED);
	}
	return ret;
}

static int ps_provider_check(void *ctx, u64 generation, u64 token)
{
	struct pstore_screen_display_handle *provider = ctx;

	if (atomic_read(&provider->state) != PS_PROVIDER_STATE_CLAIMED)
		return -ENODEV;
	if (!provider->public_ops.check)
		return 0;
	return provider->public_ops.check(provider->public_ctx, generation,
					  token);
}

static int ps_provider_enter(void *ctx, u64 generation, u64 token)
{
	struct pstore_screen_display_handle *provider = ctx;

	if (atomic_read(&provider->state) != PS_PROVIDER_STATE_CLAIMED)
		return -ENODEV;
	if (!provider->public_ops.enter)
		return 0;
	return provider->public_ops.enter(provider->public_ctx, generation,
					  token);
}

static void ps_provider_release_token(void *ctx, u64 generation, u64 token)
{
	struct pstore_screen_display_handle *provider = ctx;

	if (provider->public_ops.release)
		provider->public_ops.release(provider->public_ctx, generation,
					     token);
	if (atomic_cmpxchg(&provider->state, PS_PROVIDER_STATE_CLAIMED,
			   READ_ONCE(provider->active) ?
			   PS_PROVIDER_STATE_PUBLISHED :
			   PS_PROVIDER_STATE_RETIRED) == PS_PROVIDER_STATE_CLAIMED &&
	    !READ_ONCE(provider->active))
		atomic_cmpxchg(&provider->state, PS_PROVIDER_STATE_PUBLISHED,
			       PS_PROVIDER_STATE_RETIRED);
}

static int ps_provider_desc_validate(
		const struct pstore_screen_display_desc *desc)
{
	if (!desc || !desc->name || !desc->name[0] || !desc->stage ||
	    !desc->priority ||
	    (desc->flags & ~PSTORE_SCREEN_PROVIDER_ALLOWED_FLAGS) ||
	    (desc->capabilities &
	     ~PSTORE_SCREEN_PROVIDER_CAPABILITIES_MASK) ||
	    (!desc->ops.enter &&
	     !(desc->flags & PSTORE_SCREEN_PROVIDER_ACTIVE_SCANOUT)) ||
	    (desc->ops.reserve &&
	     (!(desc->capabilities &
		PSTORE_SCREEN_PROVIDER_CAP_RESERVE_NO_SIDE_EFFECT) ||
	      !desc->ops.release)) ||
	    (desc->ops.check &&
	     !(desc->capabilities &
	       PSTORE_SCREEN_PROVIDER_CAP_CHECK_NO_SIDE_EFFECT)) ||
	    (desc->ops.enter &&
	     !(desc->capabilities &
	       PSTORE_SCREEN_PROVIDER_CAP_ENTER_PANIC_SAFE)) ||
	    (desc->scanout.write_span &&
	     !(desc->capabilities &
	       PSTORE_SCREEN_PROVIDER_CAP_WRITE_SPAN_PANIC_SAFE)) ||
	    (desc->scanout.flush &&
	     !(desc->capabilities &
	       PSTORE_SCREEN_PROVIDER_CAP_FLUSH_PANIC_SAFE)) ||
	    (desc->ops.release &&
	     !(desc->capabilities &
	       PSTORE_SCREEN_PROVIDER_CAP_RELEASE_PANIC_SAFE)))
		return -EINVAL;
	if (desc->scanout.map_kind == PSTORE_SCREEN_MAP_WRITE_SPAN &&
	    !desc->scanout.write_span)
		return -EINVAL;
	return 0;
}

static int ps_provider_desc_snapshot(
		const struct pstore_screen_display_desc *desc,
		struct pstore_screen_display_desc *snapshot)
{
	size_t copy_size;

	if (!desc || !snapshot ||
	    desc->size < PSTORE_SCREEN_DISPLAY_DESC_V1_SIZE ||
	    desc->version < PSTORE_SCREEN_DISPLAY_DESC_V1_VERSION)
		return -EINVAL;
	if (desc->version > PSTORE_SCREEN_DISPLAY_DESC_V1_VERSION)
		return -EOPNOTSUPP;
	memset(snapshot, 0, sizeof(*snapshot));
	copy_size = min_t(size_t, (size_t)desc->size, sizeof(*snapshot));
	memcpy(snapshot, desc, copy_size);
	snapshot->capabilities &= PSTORE_SCREEN_PROVIDER_CAPABILITIES_MASK;
	return ps_provider_desc_validate(snapshot);
}

static void ps_provider_build_scanout(
		struct pstore_screen_display_handle *provider)
{
	const struct pstore_screen_scanout_desc *source =
		&provider->public_scanout;

	provider->scanout.map_kind = source->map_kind;
	provider->scanout.format = source->format;
	provider->scanout.flags = source->flags;
	provider->scanout.width = source->width;
	provider->scanout.height = source->height;
	provider->scanout.pitch = source->pitch;
	provider->scanout.length = source->length;
	switch (source->map_kind) {
	case PSTORE_SCREEN_MAP_SYSTEM:
		provider->scanout.address.system = source->address.system;
		break;
	case PSTORE_SCREEN_MAP_IOMEM:
		provider->scanout.address.io = source->address.io;
		break;
	case PSTORE_SCREEN_MAP_WRITE_SPAN:
		break;
	}
	provider->scanout.ctx = provider;
	if (source->write_span)
		provider->scanout.write_span = ps_provider_write_span;
	if (source->flush)
		provider->scanout.flush = ps_provider_flush;
}

int pstore_screen_display_register(
		const struct pstore_screen_display_desc *desc,
		struct pstore_screen_display_handle **handle)
{
	struct pstore_screen_display_handle *provider;
	struct pstore_screen_display_desc snapshot;
	struct ps_layout_request request;
	u32 slot;
	int ret;

	if (!handle)
		return -EINVAL;
	*handle = NULL;
	ret = ps_provider_desc_snapshot(desc, &snapshot);
	if (ret)
		return ret;
	desc = &snapshot;
	provider = kzalloc(sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;
	if (desc->owner && !try_module_get(desc->owner)) {
		ret = -ENODEV;
		goto err_free;
	}
	strscpy(provider->name, desc->name, sizeof(provider->name));
	provider->owner = desc->owner;
	provider->public_scanout = desc->scanout;
	provider->public_ops = desc->ops;
	provider->public_ctx = desc->ctx;
	ps_provider_build_scanout(provider);
	ret = ps_scanout_validate(&provider->scanout);
	if (ret)
		goto err_free;
	memset(&request, 0, sizeof(request));
	request.width_px = desc->scanout.width;
	request.height_px = desc->scanout.height;
	request.width_mm = desc->scanout.width_mm;
	request.height_mm = desc->scanout.height_mm;
	request.margin_x_percent = 5U;
	request.margin_y_percent = 5U;
	ret = ps_layout_choose(&request, &provider->layout);
	if (ret)
		goto err_free;
	provider->candidate.name = provider->name;
	provider->candidate.priority = desc->priority;
	provider->candidate.stage = desc->stage;
	provider->candidate.flags = desc->flags &
		PSTORE_SCREEN_PROVIDER_ACTIVE_SCANOUT ?
		PS_DISPLAY_PROVIDER_FLAG_ACTIVE_SCANOUT : 0U;
	provider->candidate.generation =
		(u64)atomic64_inc_return(&ps_display_generation);
	provider->candidate.scanout = &provider->scanout;
	provider->candidate.ctx = provider;
	provider->candidate.reserve = ps_provider_reserve;
	if (desc->ops.check)
		provider->candidate.check = ps_provider_check;
	if (desc->ops.enter)
		provider->candidate.enter = ps_provider_enter;
	provider->candidate.release = ps_provider_release_token;
	provider->candidate.capabilities =
		PS_DISPLAY_PROVIDER_CAP_RESERVE_NO_SIDE_EFFECT |
		PS_DISPLAY_PROVIDER_CAP_RELEASE_PANIC_SAFE;
	if (desc->ops.check)
		provider->candidate.capabilities |=
			PS_DISPLAY_PROVIDER_CAP_CHECK_NO_SIDE_EFFECT;
	if (desc->ops.enter)
		provider->candidate.capabilities |=
			PS_DISPLAY_PROVIDER_CAP_ENTER_PANIC_SAFE;
	if (desc->scanout.write_span)
		provider->candidate.capabilities |=
			PS_DISPLAY_PROVIDER_CAP_WRITE_SPAN_PANIC_SAFE;
	if (desc->scanout.flush)
		provider->candidate.capabilities |=
			PS_DISPLAY_PROVIDER_CAP_FLUSH_PANIC_SAFE;
	ret = ps_display_candidate_validate(&provider->candidate);
	if (ret)
		goto err_free;
	refcount_set(&provider->refs, 1);
	atomic_set(&provider->state, PS_PROVIDER_STATE_PUBLISHED);
	WRITE_ONCE(provider->active, 1U);
	mutex_lock(&ps_display_registry_lock);
	for (slot = 0U; slot < PSTORE_SCREEN_DISPLAY_MAX_PROVIDERS; slot++) {
		if (!rcu_access_pointer(ps_display_registry[slot]))
			break;
	}
	if (slot == PSTORE_SCREEN_DISPLAY_MAX_PROVIDERS) {
		mutex_unlock(&ps_display_registry_lock);
		ret = -ENOSPC;
		goto err_free;
	}
	provider->slot = slot;
	rcu_assign_pointer(ps_display_registry[slot], provider);
	mutex_unlock(&ps_display_registry_lock);
	*handle = provider;
	return 0;

err_free:
	if (provider->owner)
		module_put(provider->owner);
	kfree(provider);
	return ret;
}
EXPORT_SYMBOL_GPL(pstore_screen_display_register);

static void ps_provider_put(struct pstore_screen_display_handle *provider)
{
	if (refcount_dec_and_test(&provider->refs))
		return;
}

void pstore_screen_display_unregister(
		struct pstore_screen_display_handle *provider)
{
	if (!provider)
		return;
	mutex_lock(&ps_display_registry_lock);
	WRITE_ONCE(provider->active, 0U);
	atomic_cmpxchg(&provider->state, PS_PROVIDER_STATE_PUBLISHED,
		       PS_PROVIDER_STATE_RETIRED);
	if (rcu_access_pointer(ps_display_registry[provider->slot]) == provider)
		RCU_INIT_POINTER(ps_display_registry[provider->slot], NULL);
	mutex_unlock(&ps_display_registry_lock);
	synchronize_rcu();
	ps_provider_put(provider);
	while (refcount_read(&provider->refs))
		msleep(20U);
	smp_acquire__after_ctrl_dep();
	if (provider->owner)
		module_put(provider->owner);
	kfree(provider);
}
EXPORT_SYMBOL_GPL(pstore_screen_display_unregister);

static int ps_provider_get(struct pstore_screen_display_handle *provider)
{
	if (!READ_ONCE(provider->active) ||
	    atomic_read(&provider->state) != PS_PROVIDER_STATE_PUBLISHED ||
	    !refcount_inc_not_zero(&provider->refs))
		return 0;
	if (!READ_ONCE(provider->active) ||
	    atomic_read(&provider->state) != PS_PROVIDER_STATE_PUBLISHED) {
		ps_provider_put(provider);
		return 0;
	}
	return 1;
}

int pstore_screen_display_preclaim_internal(
		struct pstore_screen_kernel_claim *claim)
{
	struct pstore_screen_display_handle *providers[
		PSTORE_SCREEN_DISPLAY_MAX_PROVIDERS];
	struct ps_display_candidate *candidates[
		PSTORE_SCREEN_DISPLAY_MAX_PROVIDERS];
	u32 count = 0U;
	u32 slot;
	int ret;

	if (!claim)
		return -EINVAL;
	memset(claim, 0, sizeof(*claim));
	rcu_read_lock();
	for (slot = 0U; slot < PSTORE_SCREEN_DISPLAY_MAX_PROVIDERS; slot++) {
		struct pstore_screen_display_handle *provider;

		provider = rcu_dereference(ps_display_registry[slot]);
		if (!provider || !ps_provider_get(provider))
			continue;
		providers[count] = provider;
		candidates[count] = &provider->candidate;
		count++;
	}
	rcu_read_unlock();
	if (!count)
		return -ENODEV;
	ret = ps_display_select(candidates, count, &claim->core);
	if (!ret)
		claim->provider = container_of(claim->core.selected,
					       struct pstore_screen_display_handle,
					       candidate);
	for (slot = 0U; slot < count; slot++) {
		if (!ret && providers[slot] == claim->provider)
			continue;
		ps_provider_put(providers[slot]);
	}
	return ret;
}

int pstore_screen_display_begin_internal(
		struct pstore_screen_kernel_claim *claim)
{
	if (!claim || !claim->provider)
		return -EINVAL;
	return ps_display_begin(&claim->core, &claim->draw_ops);
}

int pstore_screen_display_present_internal(
		struct pstore_screen_kernel_claim *claim)
{
	if (!claim || !claim->provider)
		return -EINVAL;
	return ps_display_present(&claim->core);
}

void pstore_screen_display_mark_lost_internal(
		struct pstore_screen_kernel_claim *claim)
{
	if (claim)
		ps_display_mark_lost(&claim->core);
}

int pstore_screen_display_abort_internal(
		struct pstore_screen_kernel_claim *claim)
{
	int ret;

	if (!claim || !claim->provider)
		return -EINVAL;
	ret = ps_display_abort(&claim->core);
	if (ret)
		return ret;
	ps_provider_put(claim->provider);
	memset(claim, 0, sizeof(*claim));
	return 0;
}

const struct ps_layout *pstore_screen_display_layout_internal(
		const struct pstore_screen_kernel_claim *claim)
{
	if (!claim || !claim->provider)
		return NULL;
	return &claim->provider->layout;
}
