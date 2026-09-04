// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_core.h"

int ps_display_candidate_validate(const struct ps_display_candidate *candidate)
{
	if (!candidate || !candidate->name || !candidate->name[0] ||
	    !candidate->generation || !candidate->scanout ||
	    (candidate->flags & ~PS_DISPLAY_PROVIDER_FLAGS_ALLOWED_MASK) ||
	    (candidate->capabilities &
	     ~PS_DISPLAY_PROVIDER_CAPABILITIES_MASK) ||
	    (!candidate->enter &&
	     !(candidate->flags & PS_DISPLAY_PROVIDER_FLAG_ACTIVE_SCANOUT)) ||
	    (candidate->reserve &&
	     (!(candidate->capabilities &
		PS_DISPLAY_PROVIDER_CAP_RESERVE_NO_SIDE_EFFECT) ||
	      !candidate->release)) ||
	    (candidate->check &&
	     !(candidate->capabilities &
	       PS_DISPLAY_PROVIDER_CAP_CHECK_NO_SIDE_EFFECT)) ||
	    (candidate->enter &&
	     !(candidate->capabilities &
	       PS_DISPLAY_PROVIDER_CAP_ENTER_PANIC_SAFE)) ||
	    (candidate->scanout->write_span &&
	     !(candidate->capabilities &
	       PS_DISPLAY_PROVIDER_CAP_WRITE_SPAN_PANIC_SAFE)) ||
	    (candidate->scanout->flush &&
	     !(candidate->capabilities &
	       PS_DISPLAY_PROVIDER_CAP_FLUSH_PANIC_SAFE)) ||
	    (candidate->release &&
	     !(candidate->capabilities &
	       PS_DISPLAY_PROVIDER_CAP_RELEASE_PANIC_SAFE)))
		return -EINVAL;
	return ps_scanout_validate(candidate->scanout);
}

static int ps_candidate_before(const struct ps_display_candidate *left,
			       const struct ps_display_candidate *right)
{
	if (left->priority != right->priority)
		return left->priority > right->priority;
	if (left->generation != right->generation)
		return left->generation > right->generation;
	return left->stage > right->stage;
}

static int ps_display_draw_allowed(const struct ps_display_claim *claim)
{
	if (!claim || !claim->selected)
		return -EINVAL;
	if (claim->state == PS_DISPLAY_STATE_COMMIT_STARTED ||
	    claim->state == PS_DISPLAY_STATE_ENTERED)
		return 0;
	if (claim->state == PS_DISPLAY_STATE_LOST_PREENTER ||
	    claim->state == PS_DISPLAY_STATE_DISPLAY_LOST)
		return -ENODEV;
	return -EINVAL;
}

static int ps_display_fill_rect(void *ctx, u32 x, u32 y, u32 width,
				u32 height, u32 color)
{
	struct ps_display_claim *claim = ctx;
	int ret;

	ret = ps_display_draw_allowed(claim);
	if (ret)
		return ret;
	ret = claim->scanout_ops.fill_rect(claim->scanout_ops.ctx, x, y,
					   width, height, color);
	if (ret)
		ps_display_mark_lost(claim);
	return ret;
}

static int ps_display_blit_mono(void *ctx, u32 x, u32 y, const u8 *data,
				u32 width, u32 height, u32 pitch, u32 scale,
				u32 color)
{
	struct ps_display_claim *claim = ctx;
	int ret;

	ret = ps_display_draw_allowed(claim);
	if (ret)
		return ret;
	ret = claim->scanout_ops.blit_mono(claim->scanout_ops.ctx, x, y, data,
					   width, height, pitch, scale, color);
	if (ret)
		ps_display_mark_lost(claim);
	return ret;
}

int ps_display_select(struct ps_display_candidate *const *candidates, u32 count,
		      struct ps_display_claim *claim)
{
	struct ps_display_candidate *ordered[PS_DISPLAY_MAX_CANDIDATES];
	u32 ordered_count = 0U;
	u32 index;

	if (!candidates || !claim || !count ||
	    count > PS_DISPLAY_MAX_CANDIDATES)
		return -EINVAL;
	memset(claim, 0, sizeof(*claim));
	for (index = 0U; index < count; index++) {
		struct ps_display_candidate *candidate = candidates[index];
		u32 insert;

		if (ps_display_candidate_validate(candidate))
			continue;
		insert = ordered_count;
		while (insert > 0U &&
		       ps_candidate_before(candidate, ordered[insert - 1U])) {
			ordered[insert] = ordered[insert - 1U];
			insert--;
		}
		ordered[insert] = candidate;
		ordered_count++;
	}
	for (index = 0U; index < ordered_count; index++) {
		struct ps_display_candidate *candidate = ordered[index];
		u64 token = 0U;
		int ret = 0;

		if (candidate->reserve)
			ret = candidate->reserve(candidate->ctx, candidate->generation,
						 &token);
		if (ret)
			continue;
		claim->selected = candidate;
		claim->token = token;
		claim->state = PS_DISPLAY_STATE_RESERVED;
		claim->reservation_live = 1U;
		return 0;
	}
	return -ENODEV;
}

int ps_display_begin(struct ps_display_claim *claim, struct ps_draw_ops *ops)
{
	struct ps_display_candidate *candidate;
	int ret;

	if (!claim || !ops || claim->state != PS_DISPLAY_STATE_RESERVED ||
	    !claim->selected || !claim->reservation_live)
		return -EINVAL;
	memset(ops, 0, sizeof(*ops));
	memset(&claim->scanout_ops, 0, sizeof(claim->scanout_ops));
	candidate = claim->selected;
	if (candidate->check) {
		ret = candidate->check(candidate->ctx, candidate->generation,
				       claim->token);
		if (ret) {
			claim->state = PS_DISPLAY_STATE_LOST_PREENTER;
			return ret;
		}
	}
	claim->state = PS_DISPLAY_STATE_COMMIT_STARTED;
	if (candidate->enter) {
		ret = candidate->enter(candidate->ctx, candidate->generation,
				       claim->token);
		if (ret) {
			claim->state = PS_DISPLAY_STATE_LOST_PREENTER;
			return ret;
		}
	}
	ret = ps_scanout_draw_ops(candidate->scanout, &claim->scanout_ops);
	if (ret) {
		claim->state = PS_DISPLAY_STATE_LOST_PREENTER;
		return ret;
	}
	ops->ctx = claim;
	ops->fill_rect = ps_display_fill_rect;
	ops->blit_mono = ps_display_blit_mono;
	return 0;
}

int ps_display_present(struct ps_display_claim *claim)
{
	u32 first_frame;
	int ret;

	if (!claim || !claim->selected)
		return -EINVAL;
	if (claim->state == PS_DISPLAY_STATE_LOST_PREENTER ||
	    claim->state == PS_DISPLAY_STATE_DISPLAY_LOST)
		return -ENODEV;
	if (claim->state != PS_DISPLAY_STATE_COMMIT_STARTED &&
	    claim->state != PS_DISPLAY_STATE_ENTERED)
		return -EINVAL;
	first_frame = claim->state == PS_DISPLAY_STATE_COMMIT_STARTED;
	ret = ps_scanout_flush(claim->selected->scanout);
	if (ret) {
		claim->state = first_frame ? PS_DISPLAY_STATE_LOST_PREENTER :
			PS_DISPLAY_STATE_DISPLAY_LOST;
		return ret;
	}
	if (first_frame)
		claim->state = PS_DISPLAY_STATE_ENTERED;
	return 0;
}

void ps_display_mark_lost(struct ps_display_claim *claim)
{
	if (!claim)
		return;
	if (claim->state == PS_DISPLAY_STATE_ENTERED)
		claim->state = PS_DISPLAY_STATE_DISPLAY_LOST;
	else if (claim->state == PS_DISPLAY_STATE_RESERVED ||
		 claim->state == PS_DISPLAY_STATE_COMMIT_STARTED)
		claim->state = PS_DISPLAY_STATE_LOST_PREENTER;
}

int ps_display_abort(struct ps_display_claim *claim)
{
	if (!claim)
		return -EINVAL;
	if (claim->state == PS_DISPLAY_STATE_ENTERED ||
	    claim->state == PS_DISPLAY_STATE_DISPLAY_LOST)
		return -EBUSY;
	if (claim->state != PS_DISPLAY_STATE_RESERVED &&
	    claim->state != PS_DISPLAY_STATE_COMMIT_STARTED &&
	    claim->state != PS_DISPLAY_STATE_LOST_PREENTER)
		return -EINVAL;
	if (claim->reservation_live && claim->selected &&
	    claim->selected->release)
		claim->selected->release(claim->selected->ctx,
					 claim->selected->generation, claim->token);
	memset(claim, 0, sizeof(*claim));
	return 0;
}
