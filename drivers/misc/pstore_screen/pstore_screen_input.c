// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_core.h"

static u32 ps_input_step(u32 visible_rows)
{
	return visible_rows > 0x7fffffffU ? 0x7fffffffU : visible_rows;
}

static int ps_input_render(const struct ps_input_ops *ops, void *ctx,
			   const struct ps_input_state *state, u32 *action)
{
	int ret;

	if (!ops || !ops->render)
		return -EOPNOTSUPP;
	ret = ops->render(ctx, state->mode,
			   state->mode == PS_INPUT_MODE_TEXT ?
			   state->text_top : state->qr_page);
	if (ret)
		return ret;
	if (action)
		*action = PS_INPUT_ACTION_RENDER;
	return 0;
}

static int ps_input_power_action(struct ps_input_state *state, u32 action,
				 const struct ps_input_ops *ops, void *ctx,
				 u32 *result)
{
	int ret;

	if (!ops)
		return -EOPNOTSUPP;
	if (action == PS_INPUT_ACTION_POWER_OFF) {
		if (!ops->poweroff)
			ret = -EOPNOTSUPP;
		else
			ret = ops->poweroff(ctx);
	} else {
		if (!ops->restart)
			ret = -EOPNOTSUPP;
		else
			ret = ops->restart(ctx);
	}
	if (ret) {
		if (result)
			*result = action == PS_INPUT_ACTION_POWER_OFF ?
				PS_INPUT_ACTION_POWER_OFF_FAILED :
				PS_INPUT_ACTION_RESTART_FAILED;
		return ret;
	}
	state->active = 0U;
	if (result)
		*result = action;
	return 0;
}

static int ps_input_deadline_reached(u64 now_ms, u64 deadline_ms)
{
	return (s64)(now_ms - deadline_ms) >= 0;
}

static int ps_input_handle_deadline(struct ps_input_state *state, u64 now_ms,
				    const struct ps_input_ops *ops, void *ctx,
				    u32 *action)
{
	u32 click_count;

	if (!state->click_count || state->power_down ||
	    !ps_input_deadline_reached(now_ms, state->click_deadline_ms))
		return 0;
	click_count = state->click_count;
	state->click_count = 0U;
	state->click_deadline_ms = 0U;
	if (click_count != 1U)
		return 0;
	state->mode = state->mode == PS_INPUT_MODE_TEXT ?
		PS_INPUT_MODE_QR : PS_INPUT_MODE_TEXT;
	state->text_top = 0U;
	state->qr_page = 0U;
	return ps_input_render(ops, ctx, state, action);
}

static int ps_input_handle_long_press(struct ps_input_state *state,
				      u64 now_ms, const struct ps_input_ops *ops,
				      void *ctx, u32 *action)
{
	if (!state->power_down || state->long_fired ||
	    now_ms < state->press_started_ms ||
	    now_ms - state->press_started_ms < PS_INPUT_LONG_PRESS_MS)
		return 0;
	state->long_fired = 1U;
	state->click_count = 0U;
	state->click_deadline_ms = 0U;
	return ps_input_power_action(state, PS_INPUT_ACTION_RESTART, ops, ctx,
				    action);
}

static int ps_input_volume(struct ps_input_state *state, u32 key,
				   const struct ps_input_ops *ops, void *ctx,
				   u32 *action)
{
	u32 step = ps_input_step(state->visible_rows);
	s32 delta;

	if (key == PS_INPUT_KEY_VOLUME_UP)
		delta = -(s32)step;
	else
		delta = (s32)step;
	if (state->mode == PS_INPUT_MODE_TEXT)
		state->text_top = ps_text_scroll(state->text_top,
						 state->text_rows,
						 state->visible_rows, delta);
	else
		state->qr_page = ps_qr_page_move(state->qr_page,
						 state->qr_pages, delta < 0 ? -1 : 1);
	return ps_input_render(ops, ctx, state, action);
}

void ps_input_init(struct ps_input_state *state)
{
	if (state)
		memset(state, 0, sizeof(*state));
}

int ps_input_enter(struct ps_input_state *state, u32 text_rows,
		   u32 visible_rows, u32 qr_pages, const struct ps_input_ops *ops,
		   void *ctx, u64 now_ms, u32 *action)
{
	if (!state || !ops || !ops->render || !visible_rows)
		return -EINVAL;
	ps_input_init(state);
	state->active = 1U;
	state->mode = PS_INPUT_MODE_TEXT;
	state->text_rows = text_rows;
	state->visible_rows = visible_rows;
	state->qr_pages = qr_pages;
	state->click_deadline_ms = now_ms;
	if (action)
		*action = PS_INPUT_ACTION_NONE;
	return ps_input_render(ops, ctx, state, action);
}

int ps_input_tick(struct ps_input_state *state, u64 now_ms,
		  const struct ps_input_ops *ops, void *ctx, u32 *action)
{
	int ret;

	if (!state || !state->active)
		return -ENODEV;
	if (action)
		*action = PS_INPUT_ACTION_NONE;
	ret = ps_input_handle_long_press(state, now_ms, ops, ctx, action);
	if (ret || (action && *action != PS_INPUT_ACTION_NONE))
		return ret;
	return ps_input_handle_deadline(state, now_ms, ops, ctx, action);
}

int ps_input_event(struct ps_input_state *state, u32 key, u32 event,
		   u64 now_ms, const struct ps_input_ops *ops, void *ctx,
		   u32 *action)
{
	int ret;

	if (!state || !state->active ||
	    (key != PS_INPUT_KEY_POWER && key != PS_INPUT_KEY_VOLUME_UP &&
	     key != PS_INPUT_KEY_VOLUME_DOWN) ||
	    (event != PS_INPUT_EVENT_PRESS && event != PS_INPUT_EVENT_RELEASE))
		return -EINVAL;
	if (action)
		*action = PS_INPUT_ACTION_NONE;
	ret = ps_input_tick(state, now_ms, ops, ctx, action);
	if (ret || (action && *action != PS_INPUT_ACTION_NONE))
		return ret;

	if (key == PS_INPUT_KEY_POWER) {
		if (event == PS_INPUT_EVENT_PRESS) {
			if (!state->power_down) {
				state->power_down = 1U;
				state->long_fired = 0U;
				state->press_started_ms = now_ms;
			}
			return 0;
		}
		if (!state->power_down)
			return 0;
		state->power_down = 0U;
		if (state->long_fired)
			return 0;
		if (now_ms >= state->press_started_ms &&
		    now_ms - state->press_started_ms >= PS_INPUT_LONG_PRESS_MS) {
			state->long_fired = 1U;
			return ps_input_power_action(state, PS_INPUT_ACTION_RESTART, ops,
						    ctx, action);
		}
		if (state->click_count < 3U)
			state->click_count++;
		state->click_deadline_ms = now_ms + PS_INPUT_CLICK_WINDOW_MS;
		if (state->click_count == 3U) {
			state->click_count = 0U;
			state->click_deadline_ms = 0U;
			return ps_input_power_action(state, PS_INPUT_ACTION_POWER_OFF,
						     ops, ctx, action);
		}
		return 0;
	}
	if (event == PS_INPUT_EVENT_PRESS)
		return ps_input_volume(state, key, ops, ctx, action);
	return 0;
}
