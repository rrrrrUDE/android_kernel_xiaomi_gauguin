/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PSTORE_SCREEN_CORE_H
#define _PSTORE_SCREEN_CORE_H

#ifdef PSTORE_SCREEN_HOST
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
#else
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#endif

#define PS_ARRAY_SIZE(a) ((u32)(sizeof(a) / sizeof((a)[0])))

#define PS_TITLE_ROWS 3U
#define PS_HEADER_PSLC_STATUS_ROWS 5U
#define PS_RUNTIME_STATUS_ROWS 2U
#define PS_HEADER_ROWS 10U
#define PS_QR_FOOTER_ROWS 3U
#define PS_QR_FOOTER_COLUMNS 20U

#define PS_FONT_FIRST 0x20U
#define PS_FONT_LAST 0x7eU
#define PS_FONT_REPLACEMENT_CODEPOINT 0xfffdU
#define PS_FONT_GLYPH_WIDTH 8U
#define PS_FONT_GLYPH_HEIGHT 16U
#define PS_FONT_SCALE_MIN 1U
#define PS_FONT_SCALE_MAX 8U

#define PS_COLOR_BLACK 0x000000U
#define PS_COLOR_WHITE 0xffffffU
#define PS_COLOR_YELLOW 0xffff00U

#define PS_TEXT_TOKEN_MAX 10U

#define PSLC_MAGIC_U32 0x434c5350U
#define PSLC_VERSION 1U
#define PSLC_HEADER_LEN 32U
#define PSLC_SEGMENT_HEADER_LEN 48U
#define PSLC_MIN_TOTAL_LEN 128U

#define PSLC_CONTAINER_FLAG_TRUNCATED 0x00000001U
#define PSLC_CONTAINER_FLAG_RAMOOPS_UNAVAILABLE 0x00000002U
#define PSLC_CONTAINER_FLAG_PSTORE_DROPPED 0x00000004U
#define PSLC_CONTAINER_FLAG_CURRENT_KMSG_EMPTY 0x00000008U
#define PSLC_CONTAINER_FLAG_MIRROR_INCOMPLETE 0x00000010U
#define PSLC_CONTAINER_FLAG_MIRROR_OVERFLOW 0x00000020U
#define PSLC_CONTAINER_FLAG_MIRROR_GENERATION_GAP 0x00000040U
#define PSLC_CONTAINER_FLAG_MIRROR_COPY_FAILURE 0x00000080U
#define PSLC_CONTAINER_FLAG_MIRROR_SCAN_INCOMPLETE 0x00000100U
#define PSLC_CONTAINER_FLAG_MIRROR_BACKEND_READ_FAILURE 0x00000200U
#define PSLC_CONTAINER_MIRROR_REASON_MASK 0x000003e0U
#define PSLC_CONTAINER_FLAGS_ALLOWED_MASK 0x000003ffU

#define PSLC_SEGMENT_KIND_PANIC_DESCRIPTION 1U
#define PSLC_SEGMENT_KIND_CURRENT_KMSG 2U
#define PSLC_SEGMENT_KIND_PSTORE_RECORD 3U

#define PSLC_SEGMENT_FLAG_TRUNCATED 0x0001U
#define PSLC_SEGMENT_FLAG_COMPRESSED 0x0002U
#define PSLC_SEGMENT_FLAG_DECOMPRESS_ERROR 0x0004U
#define PSLC_SEGMENT_FLAG_HAS_ECC 0x0008U
#define PSLC_SEGMENT_FLAG_PAYLOAD_IS_RAW_COMPRESSED 0x0010U
#define PSLC_SEGMENT_FLAGS_LOW_ALLOWED_MASK 0x001fU
#define PSLC_SEGMENT_CODEC_SHIFT 8U
#define PSLC_SEGMENT_CODEC_MASK 0xff00U

#define PSLC_CODEC_NONE 0U
#define PSLC_CODEC_DEFLATE 1U
#define PSLC_CODEC_LZO 2U
#define PSLC_CODEC_LZ4 3U
#define PSLC_CODEC_ZSTD 4U

#define PSLC_PSTORE_TYPE_NONE 0U
#define PSLC_PSTORE_TYPE_DMESG 1U
#define PSLC_PSTORE_TYPE_CONSOLE 2U
#define PSLC_PSTORE_TYPE_FTRACE 3U
#define PSLC_PSTORE_TYPE_PMSG 4U
#define PSLC_PSTORE_TYPE_MCE 5U
#define PSLC_PSTORE_TYPE_PPC_RTAS 6U
#define PSLC_PSTORE_TYPE_PPC_OF 7U
#define PSLC_PSTORE_TYPE_PPC_COMMON 8U
#define PSLC_PSTORE_TYPE_PPC_OPAL 9U
#define PSLC_PSTORE_TYPE_UNKNOWN 0xffffffffU

#define PS_MIRROR_SOURCE_ABSENT 0U
#define PS_MIRROR_SOURCE_CERTIFIED_COMPLETE 1U
#define PS_MIRROR_SOURCE_CERTIFIED_INCOMPLETE 2U

#define PS_MIRROR_EVENT_OVERFLOW 0x01U
#define PS_MIRROR_EVENT_GENERATION_GAP 0x02U
#define PS_MIRROR_EVENT_COPY_FAILURE 0x04U
#define PS_MIRROR_EVENT_SCAN_INCOMPLETE 0x08U
#define PS_MIRROR_EVENT_BACKEND_READ_FAILURE 0x10U
#define PS_MIRROR_EVENTS_ALLOWED_MASK 0x1fU

#define PSQ1_HEADER_LEN 44U
#define PSQ1_FLAG_FIRST 0x01U
#define PSQ1_FLAG_LAST 0x02U
#define PSQ1_FLAG_CONTAINER_TRUNCATED 0x04U
#define PSQ1_FLAGS_ALLOWED_MASK 0x07U

#define PS_QR_MIN_VERSION 3U
#define PS_QR_MAX_VERSION 40U
#define PS_QR_MIN_MODULE_PX 4U
#define PS_QR_QUIET_ZONE_MODULES 4U

#define PS_LAYOUT_INELIGIBLE 0U
#define PS_LAYOUT_COMPACT 1U
#define PS_LAYOUT_NORMAL 2U

#define PS_FOURCC_CODE(a, b, c, d) \
	((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

#define PS_FORMAT_RGB565 PS_FOURCC_CODE('R', 'G', '1', '6')
#define PS_FORMAT_BGR565 PS_FOURCC_CODE('B', 'G', '1', '6')
#define PS_FORMAT_XRGB8888 PS_FOURCC_CODE('X', 'R', '2', '4')
#define PS_FORMAT_XBGR8888 PS_FOURCC_CODE('X', 'B', '2', '4')
#define PS_FORMAT_ARGB8888 PS_FOURCC_CODE('A', 'R', '2', '4')
#define PS_FORMAT_ABGR8888 PS_FOURCC_CODE('A', 'B', '2', '4')

#define PS_SCANOUT_MAP_SYSTEM 1U
#define PS_SCANOUT_MAP_IOMEM 2U
#define PS_SCANOUT_MAP_WRITE_SPAN 3U

#define PS_SCANOUT_FLAG_CPU_COHERENT 0x00000001U
#define PS_SCANOUT_FLAG_WRITE_THROUGH 0x00000002U
#define PS_SCANOUT_FLAGS_ALLOWED_MASK 0x00000003U

#define PS_DISPLAY_PROVIDER_FLAG_ACTIVE_SCANOUT 0x00000001U
#define PS_DISPLAY_PROVIDER_FLAGS_ALLOWED_MASK 0x00000001U

#define PS_DISPLAY_PROVIDER_CAP_RESERVE_NO_SIDE_EFFECT (1ULL << 0)
#define PS_DISPLAY_PROVIDER_CAP_CHECK_NO_SIDE_EFFECT (1ULL << 1)
#define PS_DISPLAY_PROVIDER_CAP_ENTER_PANIC_SAFE (1ULL << 2)
#define PS_DISPLAY_PROVIDER_CAP_WRITE_SPAN_PANIC_SAFE (1ULL << 3)
#define PS_DISPLAY_PROVIDER_CAP_FLUSH_PANIC_SAFE (1ULL << 4)
#define PS_DISPLAY_PROVIDER_CAP_RELEASE_PANIC_SAFE (1ULL << 5)
#define PS_DISPLAY_PROVIDER_CAPABILITIES_MASK ((1ULL << 6) - 1ULL)

#define PS_DISPLAY_MAX_CANDIDATES 8U
#define PS_DISPLAY_STATE_EMPTY 0U
#define PS_DISPLAY_STATE_RESERVED 1U
#define PS_DISPLAY_STATE_COMMIT_STARTED 2U
#define PS_DISPLAY_STATE_ENTERED 3U
#define PS_DISPLAY_STATE_LOST_PREENTER 4U
#define PS_DISPLAY_STATE_DISPLAY_LOST 5U

/* Panic-screen input is driven by a pre-certified, non-sleeping poller. */
#define PS_INPUT_MODE_TEXT 0U
#define PS_INPUT_MODE_QR 1U
#define PS_INPUT_KEY_POWER 1U
#define PS_INPUT_KEY_VOLUME_UP 2U
#define PS_INPUT_KEY_VOLUME_DOWN 3U
#define PS_INPUT_EVENT_PRESS 1U
#define PS_INPUT_EVENT_RELEASE 2U
#define PS_INPUT_CLICK_WINDOW_MS 500U
#define PS_INPUT_LONG_PRESS_MS 1000U
#define PS_INPUT_ACTION_NONE 0U
#define PS_INPUT_ACTION_RENDER 1U
#define PS_INPUT_ACTION_POWER_OFF 2U
#define PS_INPUT_ACTION_RESTART 3U
#define PS_INPUT_ACTION_POWER_OFF_FAILED 4U
#define PS_INPUT_ACTION_RESTART_FAILED 5U

struct ps_input_ops {
	int (*render)(void *ctx, u32 mode, u32 page);
	int (*poweroff)(void *ctx);
	int (*restart)(void *ctx);
};

struct ps_input_state {
	u32 active;
	u32 mode;
	u32 text_top;
	u32 text_rows;
	u32 visible_rows;
	u32 qr_page;
	u32 qr_pages;
	u32 power_down;
	u32 long_fired;
	u32 click_count;
	u64 press_started_ms;
	u64 click_deadline_ms;
};

struct ps_text_token {
	u8 ascii[PS_TEXT_TOKEN_MAX];
	u32 ascii_len;
	u32 raw_len;
	u32 hard_break;
};

struct ps_text_row {
	u32 raw_start;
	u32 raw_end;
	u32 columns;
	u32 hard_break;
};

struct ps_text_index {
	const u8 *raw;
	u32 raw_len;
	u32 columns;
	struct ps_text_row *rows;
	u32 row_capacity;
	u32 indexed_rows;
	u32 total_rows;
	u32 soft_wraps;
	u32 hard_breaks;
	u32 complete;
};

struct ps_qr_geometry {
	u32 eligible;
	u32 version;
	u32 module_px;
	u32 frame_capacity;
	u32 payload_capacity;
	u32 slot_x;
	u32 slot_y;
	u32 slot_width;
	u32 slot_height;
	u32 square_x;
	u32 square_y;
	u32 square_side;
	u32 raster_x;
	u32 raster_y;
	u32 raster_side;
	u32 footer_y;
	u32 footer_height;
};

struct ps_layout_request {
	u32 width_px;
	u32 height_px;
	u32 width_mm;
	u32 height_mm;
	u32 margin_x_percent;
	u32 margin_y_percent;
};

struct ps_layout {
	u32 eligible;
	u32 mode;
	u32 physical_trusted;
	u32 dpi_y_q16;
	u32 glyph_height_um;
	u32 scale;
	u32 glyph_width_px;
	u32 glyph_height_px;
	u32 line_gap_px;
	u32 row_pitch_px;
	u32 section_gap_px;
	u32 margin_x_px;
	u32 margin_y_px;
	u32 safe_x;
	u32 safe_y;
	u32 safe_width_px;
	u32 safe_height_px;
	u32 columns;
	u32 body_rows;
	u32 header_height_px;
	u32 footer_height_px;
	struct ps_qr_geometry qr;
};

struct ps_pslc_segment_input {
	u32 kind;
	u32 flags;
	u32 ordinal;
	u32 pstore_type;
	u64 record_id;
	u64 timestamp_sec;
	u32 timestamp_nsec;
	u32 payload_len;
	u32 ecc_len;
	const u8 *payload;
	const u8 *ecc;
};

struct ps_pslc_build_input {
	u32 container_flags;
	u32 mirror_source;
	u32 mirror_events;
	u32 tail_capacity;
	u32 current_total_len;
	u32 pstore_total_count;
	const struct ps_pslc_segment_input *segments;
	u32 segment_count;
};

struct ps_pslc_validation_context {
	u32 mirror_source;
	u32 mirror_events;
	u32 tail_capacity;
	u32 current_full_len;
	const u8 *current_full;
};

struct ps_pslc_summary {
	u32 container_flags;
	u32 current_total_len;
	u32 pstore_total_count;
	u32 current_payload_len;
	u32 serialized_pstore_count;
	u32 segment_count;
	u32 total_len;
};

struct ps_qr_set {
	const u8 *log;
	u32 total_len;
	u32 payload_capacity;
	u32 chunk_count;
	u32 whole_crc32;
	u32 container_truncated;
	u32 version;
	u32 module_px;
	u8 log_id[8];
};

struct ps_qr_frame_info {
	u32 chunk_index;
	u32 chunk_count;
	u32 payload_offset;
	u32 payload_len;
	u32 chunk_crc32;
	u32 flags;
};

struct ps_draw_ops {
	void *ctx;
	int (*fill_rect)(void *ctx, u32 x, u32 y, u32 width, u32 height,
			 u32 color);
	int (*blit_mono)(void *ctx, u32 x, u32 y, const u8 *data,
			 u32 width, u32 height, u32 pitch, u32 scale,
			 u32 color);
};

union ps_scanout_address {
	void *system;
#ifdef PSTORE_SCREEN_HOST
	void *io;
#else
	void __iomem *io;
#endif
};

struct ps_scanout {
	u32 map_kind;
	u32 format;
	u32 flags;
	u32 width;
	u32 height;
	u32 pitch;
	u64 length;
	union ps_scanout_address address;
	void *ctx;
	int (*write_span)(void *ctx, u64 offset, const u8 *source, u32 length);
	int (*flush)(void *ctx);
};

struct ps_display_candidate {
	const char *name;
	u32 priority;
	u32 stage;
	u32 flags;
	u64 generation;
	struct ps_scanout *scanout;
	void *ctx;
	int (*reserve)(void *ctx, u64 generation, u64 *token);
	int (*check)(void *ctx, u64 generation, u64 token);
	int (*enter)(void *ctx, u64 generation, u64 token);
	void (*release)(void *ctx, u64 generation, u64 token);
	u64 capabilities;
};

struct ps_display_claim {
	struct ps_display_candidate *selected;
	u64 token;
	u32 state;
	u32 reservation_live;
	struct ps_draw_ops scanout_ops;
};

const char *ps_title_line(u32 index);
const u8 *ps_font_glyph(u8 ch);
const u8 *ps_font_glyph_codepoint(u32 codepoint);
u32 ps_font_glyph_count(void);

u32 ps_crc32_begin(void);
u32 ps_crc32_feed(u32 state, const u8 *data, u32 len);
u32 ps_crc32_end(u32 state);
u32 ps_crc32(const u8 *data, u32 len);
void ps_sha256(const u8 *data, u32 len, u8 out[32]);

int ps_text_next_token(const u8 *raw, u32 raw_len, u32 raw_offset,
		       struct ps_text_token *token);
int ps_text_display_info(const u8 *raw, u32 raw_len, u32 raw_offset,
			 u32 *display_len, u32 *codepoint);
int ps_text_project(const u8 *raw, u32 raw_len, char *out, u32 out_capacity,
		    u32 *out_len);
int ps_text_index_build(struct ps_text_index *index, const u8 *raw, u32 raw_len,
			u32 columns, struct ps_text_row *rows,
			u32 row_capacity);
int ps_text_row_get(const struct ps_text_index *index, u32 row_index,
		    struct ps_text_row *row);
int ps_text_row_project(const struct ps_text_index *index,
			const struct ps_text_row *row, char *out,
			u32 out_capacity, u32 *out_len);
u32 ps_text_scroll(u32 top_line, u32 total_rows, u32 visible_rows, s32 delta);

int ps_qr_geometry_from_safe(u32 safe_width_px, u32 safe_height_px, u32 scale,
			     struct ps_qr_geometry *geometry);
int ps_layout_choose(const struct ps_layout_request *request,
		     struct ps_layout *layout);

int ps_pslc_measure(const struct ps_pslc_build_input *input, u32 *total_len);
int ps_pslc_build(const struct ps_pslc_build_input *input, u8 *out,
		  u32 out_capacity, u32 *out_len);
int ps_pslc_validate(const u8 *data, u32 data_len,
		     const struct ps_pslc_validation_context *context,
		     struct ps_pslc_summary *summary);
int ps_pslc_status_lines(const struct ps_pslc_summary *summary, char *out,
			 u32 stride);

int ps_qr_set_prepare(struct ps_qr_set *set, const u8 *log, u32 total_len,
		      u32 pslc_container_flags,
		      const struct ps_qr_geometry *geometry);
int ps_qr_build_frame(const struct ps_qr_set *set, u32 chunk_index, u8 *out,
		      u32 out_capacity, u32 *out_len,
		      struct ps_qr_frame_info *info);
int ps_qr_format_footer(const struct ps_qr_frame_info *info, char *out,
			u32 stride);
u32 ps_qr_page_move(u32 current_page, u32 count, s32 delta);
u32 ps_qr_workbuf_len(u32 version);
u32 ps_qr_matrix_len(u32 version);
int ps_qr_encode_matrix(const u8 *frame, u32 frame_len, u32 version,
			u8 *temp, u32 temp_len, u8 *qr_work,
			u32 qr_work_len, u8 *matrix, u32 matrix_len,
			u32 *matrix_size);
int ps_qr_matrix_get(const u8 *matrix, u32 matrix_size, u32 x, u32 y);

int ps_render_begin(const struct ps_layout *layout,
		    const struct ps_draw_ops *ops);
int ps_render_header(const struct ps_layout *layout, const char *status_lines,
		     u32 status_stride, u32 runtime_flags,
		     const char *runtime_label,
		     const struct ps_draw_ops *ops);
int ps_render_text_page(const struct ps_layout *layout,
			const struct ps_text_index *index, u32 top_line,
			const struct ps_draw_ops *ops);
int ps_render_qr_page(const struct ps_layout *layout, const u8 *matrix,
		      u32 matrix_size, const char *footer_lines,
		      u32 footer_stride, const struct ps_draw_ops *ops);

int ps_pixel_pack(u32 format, u32 color, u8 out[4], u32 *out_len);
int ps_scanout_validate(const struct ps_scanout *scanout);
int ps_scanout_fill_rect(struct ps_scanout *scanout, u32 x, u32 y,
			 u32 width, u32 height, u32 color);
int ps_scanout_blit_mono(struct ps_scanout *scanout, u32 x, u32 y,
			 const u8 *data, u32 width, u32 height, u32 pitch,
			 u32 scale, u32 color);
int ps_scanout_flush(struct ps_scanout *scanout);
int ps_scanout_draw_ops(struct ps_scanout *scanout, struct ps_draw_ops *ops);

int ps_display_candidate_validate(const struct ps_display_candidate *candidate);
int ps_display_select(struct ps_display_candidate *const *candidates, u32 count,
		      struct ps_display_claim *claim);
int ps_display_begin(struct ps_display_claim *claim, struct ps_draw_ops *ops);
int ps_display_present(struct ps_display_claim *claim);
void ps_display_mark_lost(struct ps_display_claim *claim);
int ps_display_abort(struct ps_display_claim *claim);

void ps_input_init(struct ps_input_state *state);
int ps_input_enter(struct ps_input_state *state, u32 text_rows,
		   u32 visible_rows, u32 qr_pages, const struct ps_input_ops *ops,
		   void *ctx, u64 now_ms, u32 *action);
int ps_input_event(struct ps_input_state *state, u32 key, u32 event,
		   u64 now_ms, const struct ps_input_ops *ops, void *ctx,
		   u32 *action);
int ps_input_tick(struct ps_input_state *state, u64 now_ms,
		  const struct ps_input_ops *ops, void *ctx, u32 *action);

#endif
