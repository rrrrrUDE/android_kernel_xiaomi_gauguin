// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_core.h"
#include "pstore_screen_display_internal.h"
#include "pstore_screen_log_internal.h"

#ifndef PSTORE_SCREEN_HOST
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pstore_screen_panic.h>
#include <linux/sched/clock.h>
#include <linux/string.h>

#define PS_PANIC_MAX_TEXT_ROWS ((PS_LOG_TAIL_BYTES / 16U) + 4U)
#define PS_PANIC_QR_FRAME_BYTES 4096U
#define PS_PANIC_QR_WORK_BYTES 4096U
#define PS_PANIC_QR_MATRIX_BYTES 4096U
#define PS_PANIC_STATUS_STRIDE 64U

struct ps_panic_screen {
	struct pstore_screen_kernel_claim claim;
	struct ps_layout layout;
	struct ps_input_state input;
	struct ps_text_index text;
	struct ps_qr_set qr;
	struct ps_pslc_summary summary;
	struct ps_text_row rows[PS_PANIC_MAX_TEXT_ROWS];
	u8 qr_frame[PS_PANIC_QR_FRAME_BYTES];
	u8 qr_temp[PS_PANIC_QR_WORK_BYTES];
	u8 qr_work[PS_PANIC_QR_WORK_BYTES];
	u8 qr_matrix[PS_PANIC_QR_MATRIX_BYTES];
	char status[PS_HEADER_PSLC_STATUS_ROWS * PS_PANIC_STATUS_STRIDE];
	char footer[PS_QR_FOOTER_ROWS * (PS_QR_FOOTER_COLUMNS + 1U)];
	const u8 *raw;
	u32 raw_len;
	u32 started;
};

static struct ps_panic_screen ps_panic;

/* Platform code may override these with pre-certified panic-safe operations. */
int __weak pstore_screen_panic_poll_key(u32 *key, u32 *event, u64 *now_ms)
{
	return -ENOSYS;
}

int __weak pstore_screen_panic_poweroff(void)
{
	return -EOPNOTSUPP;
}

int __weak pstore_screen_panic_restart(void)
{
	return -EOPNOTSUPP;
}

static u16 ps_panic_le16(const u8 *data)
{
	return (u16)data[0] | ((u16)data[1] << 8);
}

static u32 ps_panic_le32(const u8 *data)
{
	return (u32)data[0] | ((u32)data[1] << 8) |
	       ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static int ps_panic_current_payload(const u8 *data, u32 data_len,
				    const u8 **payload, u32 *payload_len)
{
	u32 description_len;
	u32 segment_offset;
	u32 segment_len;
	u32 padding;

	if (!data || !payload || !payload_len || data_len < PSLC_MIN_TOTAL_LEN ||
	    data[0] != 'P' || data[1] != 'S' || data[2] != 'L' ||
	    data[3] != 'C' || ps_panic_le16(data + 4U) != PSLC_VERSION ||
	    ps_panic_le16(data + 6U) != PSLC_HEADER_LEN ||
	    ps_panic_le32(data + 8U) != data_len)
		return -EINVAL;
	if (data_len < PSLC_HEADER_LEN + PSLC_SEGMENT_HEADER_LEN)
		return -EINVAL;
	if (ps_panic_le16(data + PSLC_HEADER_LEN) !=
	    PSLC_SEGMENT_KIND_PANIC_DESCRIPTION ||
	    ps_panic_le16(data + PSLC_HEADER_LEN + 4U) !=
	    PSLC_SEGMENT_HEADER_LEN)
		return -EINVAL;
	description_len = ps_panic_le32(data + PSLC_HEADER_LEN + 36U);
	segment_len = PSLC_SEGMENT_HEADER_LEN + description_len;
	padding = (4U - (segment_len & 3U)) & 3U;
	if (segment_len > data_len - PSLC_HEADER_LEN ||
	    padding > data_len - PSLC_HEADER_LEN - segment_len)
		return -EINVAL;
	segment_offset = PSLC_HEADER_LEN + segment_len + padding;
	if (data_len - segment_offset < PSLC_SEGMENT_HEADER_LEN ||
	    ps_panic_le16(data + segment_offset) !=
	    PSLC_SEGMENT_KIND_CURRENT_KMSG ||
	    ps_panic_le16(data + segment_offset + 4U) !=
	    PSLC_SEGMENT_HEADER_LEN)
		return -EINVAL;
	*payload_len = ps_panic_le32(data + segment_offset + 36U);
	if (*payload_len > data_len - segment_offset - PSLC_SEGMENT_HEADER_LEN)
		return -EINVAL;
	*payload = data + segment_offset + PSLC_SEGMENT_HEADER_LEN;
	return 0;
}

static int ps_panic_render(void *ctx, u32 mode, u32 page)
{
	struct ps_panic_screen *screen = ctx;
	struct ps_qr_frame_info info;
	u32 frame_len;
	u32 matrix_size;
	int ret;

	/* The retained scanout may still contain ABL/boot-animation pixels. */
	ret = ps_render_begin(&screen->layout, &screen->claim.draw_ops);
	if (ret)
		return ret;
	ret = ps_render_header(&screen->layout, screen->status,
				       PS_PANIC_STATUS_STRIDE, screen->summary.container_flags,
				       mode == PS_INPUT_MODE_QR ? "QR" : "TEXT",
				       &screen->claim.draw_ops);
	if (ret)
		return ret;
	if (mode == PS_INPUT_MODE_TEXT)
		ret = ps_render_text_page(&screen->layout, &screen->text, page,
					  &screen->claim.draw_ops);
	else {
		ret = ps_qr_build_frame(&screen->qr, page, screen->qr_frame,
					PS_PANIC_QR_FRAME_BYTES, &frame_len, &info);
		if (ret)
			return ret;
		ret = ps_qr_encode_matrix(screen->qr_frame, frame_len,
					  screen->qr.version, screen->qr_temp,
					  PS_PANIC_QR_WORK_BYTES, screen->qr_work,
					  PS_PANIC_QR_WORK_BYTES, screen->qr_matrix,
					  PS_PANIC_QR_MATRIX_BYTES, &matrix_size);
		if (ret)
			return ret;
		ret = ps_qr_format_footer(&info, screen->footer,
					  PS_QR_FOOTER_COLUMNS + 1U);
		if (ret)
			return ret;
		ret = ps_render_qr_page(&screen->layout, screen->qr_matrix,
					matrix_size, screen->footer,
					PS_QR_FOOTER_COLUMNS + 1U,
					&screen->claim.draw_ops);
	}
	if (ret)
		return ret;
	return pstore_screen_display_present_internal(&screen->claim);
}

static int ps_panic_poweroff(void *ctx)
{
	(void)ctx;
	return pstore_screen_panic_poweroff();
}

static int ps_panic_restart(void *ctx)
{
	(void)ctx;
	return pstore_screen_panic_restart();
}

int pstore_screen_panic_start(void)
{
	const u8 *container;
	u32 container_len;
	struct ps_input_ops input_ops = {
		.render = ps_panic_render,
		.poweroff = ps_panic_poweroff,
		.restart = ps_panic_restart,
	};
	int ret;

	if (ps_panic.started)
		return 0;
	ret = pstore_screen_log_snapshot_internal(&container, &container_len,
						 &ps_panic.summary);
	if (ret)
		return ret;
	ret = pstore_screen_display_preclaim_internal(&ps_panic.claim);
	if (ret)
		return ret;
	ret = pstore_screen_display_begin_internal(&ps_panic.claim);
	if (ret)
		goto abort_claim;
	ps_panic.layout = *pstore_screen_display_layout_internal(&ps_panic.claim);
	ret = ps_panic_current_payload(container, container_len, &ps_panic.raw,
					       &ps_panic.raw_len);
	if (ret || !ps_panic.raw_len) {
		static const u8 empty_log[] = "No current kmsg";

		ps_panic.raw = empty_log;
		ps_panic.raw_len = sizeof(empty_log) - 1U;
	}
	ret = ps_text_index_build(&ps_panic.text, ps_panic.raw, ps_panic.raw_len,
				  ps_panic.layout.columns, ps_panic.rows,
				  PS_PANIC_MAX_TEXT_ROWS);
	if (ret)
		goto abort_claim;
	ret = ps_pslc_status_lines(&ps_panic.summary, ps_panic.status,
					 PS_PANIC_STATUS_STRIDE);
	if (ret)
		goto abort_claim;
	ret = ps_qr_set_prepare(&ps_panic.qr, container, container_len,
				       ps_panic.summary.container_flags,
				       &ps_panic.layout.qr);
	if (ret)
		goto abort_claim;
	ret = ps_input_enter(&ps_panic.input, ps_panic.text.total_rows,
				    ps_panic.layout.body_rows, ps_panic.qr.chunk_count,
				    &input_ops, &ps_panic, 0U, NULL);
	if (ret)
		goto abort_claim;
	ps_panic.started = 1U;
	return 0;

abort_claim:
	pstore_screen_display_abort_internal(&ps_panic.claim);
	memset(&ps_panic, 0, sizeof(ps_panic));
	return ret;
}

void pstore_screen_panic_wait(void)
{
	struct ps_input_ops input_ops = {
		.render = ps_panic_render,
		.poweroff = ps_panic_poweroff,
		.restart = ps_panic_restart,
	};

	if (!ps_panic.started)
		return;
	for (;;) {
		u32 key = 0U;
		u32 event = 0U;
		u64 now_ms = sched_clock() / 1000000ULL;
		int ret;

		ret = pstore_screen_panic_poll_key(&key, &event, &now_ms);
		if (!ret)
			ps_input_event(&ps_panic.input, key, event, now_ms,
				       &input_ops, &ps_panic, NULL);
		else if (ret != -EAGAIN && ret != -ENOSYS)
			cpu_relax();
		ps_input_tick(&ps_panic.input, now_ms, &input_ops, &ps_panic, NULL);
		cpu_relax();
	}
}

EXPORT_SYMBOL_GPL(pstore_screen_panic_start);
EXPORT_SYMBOL_GPL(pstore_screen_panic_wait);

#endif
