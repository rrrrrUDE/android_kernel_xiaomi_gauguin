// SPDX-License-Identifier: GPL-2.0-only
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_pstore_screen.h>

#include <linux/module.h>
#include <linux/build_bug.h>
#include <linux/overflow.h>
#include <linux/pstore_screen.h>
#include <linux/slab.h>

struct pstore_screen_drm_sidecar {
	struct drm_device *drm;
	struct drm_framebuffer *framebuffer;
	struct drm_gem_object *gem;
	struct pstore_screen_display_handle *display;
	struct pstore_screen_drm_sidecar_desc desc;
	struct module *callback_owner;
	u64 offset;
};

static_assert(sizeof(struct pstore_screen_drm_sidecar_desc) ==
	      PSTORE_SCREEN_DRM_SIDECAR_DESC_V1_SIZE,
	      "pstore screen DRM sidecar v1 layout must remain frozen");

static int ps_drm_offset_address(
		const struct pstore_screen_drm_sidecar_desc *desc,
		u64 offset, unsigned long *result)
{
	unsigned long base;

	if (!result || offset > (u64)~0UL)
		return -EOVERFLOW;
	if (desc->map_kind == PSTORE_SCREEN_MAP_SYSTEM)
		base = (unsigned long)desc->address.system;
	else if (desc->map_kind == PSTORE_SCREEN_MAP_IOMEM)
		base = (__force unsigned long)desc->address.io;
	else
		return -EINVAL;
	if (check_add_overflow(base, (unsigned long)offset, result))
		return -EOVERFLOW;
	return 0;
}

static int ps_drm_write_span(void *ctx, u64 offset, const void *source,
			     u32 length)
{
	struct pstore_screen_drm_sidecar *sidecar = ctx;

	return sidecar->desc.write_span(sidecar->desc.ctx,
					 sidecar->offset + offset, source, length);
}

static int ps_drm_flush(void *ctx)
{
	struct pstore_screen_drm_sidecar *sidecar = ctx;

	return sidecar->desc.flush(sidecar->desc.ctx);
}

static int ps_drm_reserve(void *ctx, u64 generation, u64 *token)
{
	struct pstore_screen_drm_sidecar *sidecar = ctx;

	if (!sidecar->desc.ops.reserve) {
		*token = 0U;
		return 0;
	}
	return sidecar->desc.ops.reserve(sidecar->desc.ctx, generation, token);
}

static int ps_drm_check(void *ctx, u64 generation, u64 token)
{
	struct pstore_screen_drm_sidecar *sidecar = ctx;

	if (!sidecar->desc.ops.check)
		return 0;
	return sidecar->desc.ops.check(sidecar->desc.ctx, generation, token);
}

static int ps_drm_enter(void *ctx, u64 generation, u64 token)
{
	struct pstore_screen_drm_sidecar *sidecar = ctx;

	if (!sidecar->desc.ops.enter)
		return 0;
	return sidecar->desc.ops.enter(sidecar->desc.ctx, generation, token);
}

static void ps_drm_release(void *ctx, u64 generation, u64 token)
{
	struct pstore_screen_drm_sidecar *sidecar = ctx;

	if (sidecar->desc.ops.release)
		sidecar->desc.ops.release(sidecar->desc.ctx, generation, token);
}

static int ps_drm_desc_validate(
		const struct pstore_screen_drm_sidecar_desc *desc)
{
	struct drm_framebuffer *framebuffer;
	u64 required;
	u32 cpp;

	if (!desc || !desc->name || !desc->framebuffer || !desc->gem ||
	    !desc->map_length ||
	    (desc->flags & ~PSTORE_SCREEN_DRM_SIDECAR_ALLOWED_FLAGS) ||
	    (desc->flags & PSTORE_SCREEN_DRM_SIDECAR_REQUIRED_FLAGS) !=
		PSTORE_SCREEN_DRM_SIDECAR_REQUIRED_FLAGS ||
	    (desc->capabilities &
	     ~PSTORE_SCREEN_PROVIDER_CAPABILITIES_MASK) ||
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
	    (desc->write_span &&
	     !(desc->capabilities &
	       PSTORE_SCREEN_PROVIDER_CAP_WRITE_SPAN_PANIC_SAFE)) ||
	    (desc->flush &&
	     !(desc->capabilities &
	       PSTORE_SCREEN_PROVIDER_CAP_FLUSH_PANIC_SAFE)) ||
	    (desc->ops.release &&
	     !(desc->capabilities &
	       PSTORE_SCREEN_PROVIDER_CAP_RELEASE_PANIC_SAFE)))
		return -EINVAL;
#ifdef MODULE
	if (!desc->owner)
		return -EINVAL;
#endif
	framebuffer = desc->framebuffer;
	if (!framebuffer->dev || !framebuffer->format ||
	    !framebuffer->width || !framebuffer->height ||
	    framebuffer->format->num_planes != 1U ||
	    framebuffer->modifier != DRM_FORMAT_MOD_LINEAR ||
	    drm_gem_fb_get_obj(framebuffer, 0U) != desc->gem)
		return -EINVAL;
	cpp = framebuffer->format->cpp[0];
	required = (u64)framebuffer->width * cpp;
	if (!cpp || required > framebuffer->pitches[0])
		return -EINVAL;
	required = (u64)(framebuffer->height - 1U) * framebuffer->pitches[0] +
		   required;
	if (framebuffer->offsets[0] > desc->map_length ||
	    required > desc->map_length - framebuffer->offsets[0] ||
	    framebuffer->offsets[0] > desc->gem->size ||
	    required > desc->gem->size - framebuffer->offsets[0])
		return -EOVERFLOW;
	if (!(desc->flags & PSTORE_SCREEN_DRM_SIDECAR_ACTIVE_SCANOUT) &&
	    !desc->ops.enter)
		return -EINVAL;
	switch (desc->map_kind) {
	case PSTORE_SCREEN_MAP_SYSTEM:
		if (!desc->address.system)
			return -EINVAL;
		break;
	case PSTORE_SCREEN_MAP_IOMEM:
		if (!desc->address.io)
			return -EINVAL;
		break;
	case PSTORE_SCREEN_MAP_WRITE_SPAN:
		if (!desc->write_span)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int ps_drm_desc_snapshot(
		const struct pstore_screen_drm_sidecar_desc *desc,
		struct pstore_screen_drm_sidecar_desc *snapshot)
{
	size_t copy_size;

	if (!desc || !snapshot ||
	    desc->size < PSTORE_SCREEN_DRM_SIDECAR_DESC_V1_SIZE ||
	    desc->version < PSTORE_SCREEN_DRM_SIDECAR_DESC_V1_VERSION)
		return -EINVAL;
	if (desc->version > PSTORE_SCREEN_DRM_SIDECAR_DESC_V1_VERSION)
		return -EOPNOTSUPP;
	memset(snapshot, 0, sizeof(*snapshot));
	copy_size = min_t(size_t, (size_t)desc->size, sizeof(*snapshot));
	memcpy(snapshot, desc, copy_size);
	snapshot->capabilities &= PSTORE_SCREEN_PROVIDER_CAPABILITIES_MASK;
	return ps_drm_desc_validate(snapshot);
}

int pstore_screen_drm_sidecar_register(
		const struct pstore_screen_drm_sidecar_desc *desc,
		struct pstore_screen_drm_sidecar **result)
{
	struct pstore_screen_drm_sidecar *sidecar;
	struct pstore_screen_drm_sidecar_desc snapshot;
	struct pstore_screen_display_desc display;
	unsigned long scanout_address = 0UL;
	int ret;

	if (!result)
		return -EINVAL;
	*result = NULL;
	ret = ps_drm_desc_snapshot(desc, &snapshot);
	if (ret)
		return ret;
	desc = &snapshot;
	sidecar = kzalloc(sizeof(*sidecar), GFP_KERNEL);
	if (!sidecar)
		return -ENOMEM;
	if (desc->owner && !try_module_get(desc->owner)) {
		ret = -ENODEV;
		goto err_free;
	}
	sidecar->callback_owner = desc->owner;
	sidecar->desc = *desc;
	sidecar->drm = desc->framebuffer->dev;
	sidecar->framebuffer = desc->framebuffer;
	sidecar->gem = desc->gem;
	sidecar->offset = desc->framebuffer->offsets[0];
	drm_dev_get(sidecar->drm);
	drm_framebuffer_get(sidecar->framebuffer);
	drm_gem_object_get(sidecar->gem);
	if (desc->map_kind != PSTORE_SCREEN_MAP_WRITE_SPAN) {
		ret = ps_drm_offset_address(desc, sidecar->offset,
					    &scanout_address);
		if (ret)
			goto err_refs;
	}
	memset(&display, 0, sizeof(display));
	display.size = sizeof(display);
	display.version = PSTORE_SCREEN_DISPLAY_DESC_VERSION;
	display.name = desc->name;
	display.owner = desc->owner;
	display.stage = PSTORE_SCREEN_STAGE_DRM;
	display.priority = desc->priority ? desc->priority :
		PSTORE_SCREEN_PRIORITY_DRM;
	display.capabilities = desc->capabilities &
		PSTORE_SCREEN_PROVIDER_CAPABILITIES_MASK;
	if (desc->flags & PSTORE_SCREEN_DRM_SIDECAR_ACTIVE_SCANOUT)
		display.flags |= PSTORE_SCREEN_PROVIDER_ACTIVE_SCANOUT;
	display.scanout.map_kind = desc->map_kind;
	display.scanout.format = desc->framebuffer->format->format;
	display.scanout.flags = desc->map_flags;
	display.scanout.width = desc->framebuffer->width;
	display.scanout.height = desc->framebuffer->height;
	display.scanout.pitch = desc->framebuffer->pitches[0];
	display.scanout.width_mm = desc->width_mm;
	display.scanout.height_mm = desc->height_mm;
	display.scanout.length = desc->map_length - sidecar->offset;
	if (desc->map_kind == PSTORE_SCREEN_MAP_SYSTEM)
		display.scanout.address.system = (void *)scanout_address;
	else if (desc->map_kind == PSTORE_SCREEN_MAP_IOMEM)
		display.scanout.address.io =
			(__force void __iomem *)scanout_address;
	display.scanout.ctx = sidecar;
	if (desc->write_span)
		display.scanout.write_span = ps_drm_write_span;
	if (desc->flush)
		display.scanout.flush = ps_drm_flush;
	display.ctx = sidecar;
	if (desc->ops.reserve)
		display.ops.reserve = ps_drm_reserve;
	if (desc->ops.check)
		display.ops.check = ps_drm_check;
	if (desc->ops.enter)
		display.ops.enter = ps_drm_enter;
	if (desc->ops.release)
		display.ops.release = ps_drm_release;
	ret = pstore_screen_display_register(&display, &sidecar->display);
	if (ret)
		goto err_refs;
	*result = sidecar;
	return 0;

err_refs:
	drm_gem_object_put(sidecar->gem);
	drm_framebuffer_put(sidecar->framebuffer);
	drm_dev_put(sidecar->drm);
	if (sidecar->callback_owner)
		module_put(sidecar->callback_owner);
	kfree(sidecar);
	return ret;

err_free:
	kfree(sidecar);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(pstore_screen_drm_sidecar_register, PSTORE_SCREEN_DRM);

void pstore_screen_drm_sidecar_unregister(
		struct pstore_screen_drm_sidecar *sidecar)
{
	if (!sidecar)
		return;
	pstore_screen_display_unregister(sidecar->display);
	drm_gem_object_put(sidecar->gem);
	drm_framebuffer_put(sidecar->framebuffer);
	drm_dev_put(sidecar->drm);
	if (sidecar->callback_owner)
		module_put(sidecar->callback_owner);
	kfree(sidecar);
}
EXPORT_SYMBOL_NS_GPL(pstore_screen_drm_sidecar_unregister, PSTORE_SCREEN_DRM);

MODULE_IMPORT_NS(PSTORE_SCREEN);
MODULE_DESCRIPTION("Pstore screen DRM stable-sidecar adapter");
MODULE_LICENSE("GPL");
