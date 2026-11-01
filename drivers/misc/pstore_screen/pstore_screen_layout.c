// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_core.h"

#define PS_PHYSICAL_GLYPH_TARGET_UM 3000U
#define PS_PHYSICAL_GLYPH_MIN_UM 2200U
#define PS_PHYSICAL_GLYPH_MAX_UM 4200U
#define PS_PHYSICAL_MM_MIN 25U
#define PS_PHYSICAL_MM_MAX 1000U
#define PS_DPI_MIN 50U
#define PS_DPI_MAX 1000U
#define PS_PHYSICAL_ERROR_PPM_MAX 50000U
#define PS_MARGIN_PERCENT_MIN 5U
#define PS_MARGIN_PERCENT_MAX 8U
#define PS_MARGIN_PX_MIN 16U
#define PS_NORMAL_COLUMNS_MIN 48U
#define PS_NORMAL_ROWS_MIN 18U
#define PS_COMPACT_COLUMNS_MIN 16U
#define PS_COMPACT_ROWS_MIN 6U
#define PS_DIMENSION_PX_MAX 65535U

static const u16 ps_qr_byte_capacity_l[PS_QR_MAX_VERSION + 1U] = {
	0U, 17U, 32U, 53U, 78U, 106U, 134U, 154U, 192U, 230U,
	271U, 321U, 367U, 425U, 458U, 520U, 586U, 644U, 718U,
	792U, 858U, 929U, 1003U, 1091U, 1171U, 1273U, 1367U,
	1465U, 1528U, 1628U, 1732U, 1840U, 1952U, 2068U, 2188U,
	2303U, 2431U, 2563U, 2699U, 2809U, 2953U,
};

static u32 ps_min_u32(u32 left, u32 right)
{
	return left < right ? left : right;
}

static u32 ps_max_u32(u32 left, u32 right)
{
	return left > right ? left : right;
}

static u64 ps_round_div_u64(u64 value, u64 divisor)
{
	u64 quotient = value / divisor;
	u64 remainder = value % divisor;

	if (remainder >= (divisor + 1U) / 2U)
		quotient++;
	return quotient;
}

static u32 ps_spacing_round(u32 value, u32 divisor, u32 minimum)
{
	u32 rounded = (u32)ps_round_div_u64(value, divisor);

	return ps_max_u32(rounded, minimum);
}

static u32 ps_fallback_dpi(u32 short_side)
{
	if (short_side <= 720U)
		return 240U;
	if (short_side <= 1080U)
		return 360U;
	if (short_side <= 1440U)
		return 480U;
	return 640U;
}

static u32 ps_margin_percent(u32 value)
{
	if (!value)
		return PS_MARGIN_PERCENT_MIN;
	if (value < PS_MARGIN_PERCENT_MIN)
		return PS_MARGIN_PERCENT_MIN;
	if (value > PS_MARGIN_PERCENT_MAX)
		return PS_MARGIN_PERCENT_MAX;
	return value;
}

static int ps_physical_dpi(const struct ps_layout_request *request,
			   u32 *dpi_y_q16)
{
	u64 dpi_x;
	u64 dpi_y;
	u64 cross_a;
	u64 cross_b;
	u64 aspect_error;
	u64 axis_error;

	if (request->width_mm < PS_PHYSICAL_MM_MIN ||
	    request->width_mm > PS_PHYSICAL_MM_MAX ||
	    request->height_mm < PS_PHYSICAL_MM_MIN ||
	    request->height_mm > PS_PHYSICAL_MM_MAX)
		return -EINVAL;

	dpi_x = ps_round_div_u64((u64)request->width_px * 25400U * 65536U,
				       (u64)request->width_mm * 1000U);
	dpi_y = ps_round_div_u64((u64)request->height_px * 25400U * 65536U,
				       (u64)request->height_mm * 1000U);
	if (dpi_x < (u64)PS_DPI_MIN * 65536U ||
	    dpi_x > (u64)PS_DPI_MAX * 65536U ||
	    dpi_y < (u64)PS_DPI_MIN * 65536U ||
	    dpi_y > (u64)PS_DPI_MAX * 65536U)
		return -EINVAL;

	cross_a = (u64)request->width_px * request->height_mm;
	cross_b = (u64)request->height_px * request->width_mm;
	aspect_error = ps_round_div_u64((cross_a > cross_b ?
					     cross_a - cross_b : cross_b - cross_a) *
					    1000000U,
					    cross_a > cross_b ? cross_a : cross_b);
	axis_error = ps_round_div_u64((dpi_x > dpi_y ? dpi_x - dpi_y :
					    dpi_y - dpi_x) * 1000000U,
					   dpi_x > dpi_y ? dpi_x : dpi_y);
	if (aspect_error > PS_PHYSICAL_ERROR_PPM_MAX ||
	    axis_error > PS_PHYSICAL_ERROR_PPM_MAX || dpi_y > 0xffffffffU)
		return -EINVAL;
	*dpi_y_q16 = (u32)dpi_y;
	return 0;
}

static u32 ps_glyph_height_um(u32 glyph_height_px, u32 dpi_y_q16)
{
	u64 value = (u64)glyph_height_px * 25400U * 65536U;

	return (u32)ps_round_div_u64(value, dpi_y_q16);
}

int ps_qr_geometry_from_safe(u32 safe_width_px, u32 safe_height_px, u32 scale,
			     struct ps_qr_geometry *geometry)
{
	u32 glyph_height;
	u32 line_gap;
	u32 row_pitch;
	u32 section_gap;
	u32 header_height;
	u32 footer_height;
	u32 square_side;
	u32 version;

	if (!geometry || scale < PS_FONT_SCALE_MIN ||
	    scale > PS_FONT_SCALE_MAX)
		return -EINVAL;
	memset(geometry, 0, sizeof(*geometry));
	glyph_height = PS_FONT_GLYPH_HEIGHT * scale;
	line_gap = ps_spacing_round(glyph_height, 8U, 1U);
	row_pitch = glyph_height + line_gap;
	section_gap = ps_spacing_round(glyph_height, 4U, 2U);
	header_height = PS_HEADER_ROWS * row_pitch + section_gap;
	footer_height = PS_QR_FOOTER_ROWS * row_pitch;
	geometry->slot_width = safe_width_px;
	geometry->footer_height = footer_height;
	if (safe_height_px < header_height + footer_height)
		return -ERANGE;
	geometry->slot_y = header_height;
	geometry->slot_height = safe_height_px - header_height - footer_height;
	geometry->footer_y = safe_height_px - footer_height;
	square_side = ps_min_u32(geometry->slot_width, geometry->slot_height);
	geometry->square_side = square_side;
	geometry->square_x = (geometry->slot_width - square_side) / 2U;
	geometry->square_y = geometry->slot_y +
		(geometry->slot_height - square_side) / 2U;
	if (safe_width_px < PS_QR_FOOTER_COLUMNS * PS_FONT_GLYPH_WIDTH * scale)
		return -ERANGE;

	for (version = PS_QR_MAX_VERSION; version >= PS_QR_MIN_VERSION;
	     version--) {
		u32 total_modules = 17U + 4U * version +
			2U * PS_QR_QUIET_ZONE_MODULES;
		u32 module_px = square_side / total_modules;
		u32 capacity = ps_qr_byte_capacity_l[version];
		u32 raster_side;

		if (module_px < PS_QR_MIN_MODULE_PX ||
		    capacity <= PSQ1_HEADER_LEN)
			continue;
		raster_side = total_modules * module_px;
		geometry->eligible = 1U;
		geometry->version = version;
		geometry->module_px = module_px;
		geometry->frame_capacity = capacity;
		geometry->payload_capacity = capacity - PSQ1_HEADER_LEN;
		geometry->raster_side = raster_side;
		geometry->raster_x = geometry->square_x +
			(square_side - raster_side) / 2U;
		geometry->raster_y = geometry->square_y +
			(square_side - raster_side) / 2U;
		return 0;
	}
	return -ERANGE;
}

static int ps_candidate_better(u32 outside, u32 error, u32 mode, u32 scale,
			       u32 best_outside, u32 best_error,
			       u32 best_mode, u32 best_scale, u32 have_best)
{
	if (!have_best || outside != best_outside)
		return !have_best || outside < best_outside;
	if (error != best_error)
		return error < best_error;
	if (mode != best_mode)
		return mode < best_mode;
	return scale > best_scale;
}

int ps_layout_choose(const struct ps_layout_request *request,
		     struct ps_layout *layout)
{
	u32 short_side;
	u32 dpi_y_q16;
	u32 physical_trusted;
	u32 margin_x;
	u32 margin_y;
	u32 safe_width;
	u32 safe_height;
	u32 best_outside = 0U;
	u32 best_error = 0U;
	u32 best_mode_score = 0U;
	u32 best_scale = 0U;
	u32 best_mode = PS_LAYOUT_INELIGIBLE;
	u32 best_glyph_um = 0U;
	u32 have_best = 0U;
	u32 scale;
	struct ps_qr_geometry best_qr;

	if (!request || !layout || !request->width_px || !request->height_px ||
	    request->width_px > PS_DIMENSION_PX_MAX ||
	    request->height_px > PS_DIMENSION_PX_MAX)
		return -EINVAL;
	memset(layout, 0, sizeof(*layout));
	memset(&best_qr, 0, sizeof(best_qr));
	short_side = ps_min_u32(request->width_px, request->height_px);
	margin_x = ps_max_u32(PS_MARGIN_PX_MIN,
		(u32)ps_round_div_u64((u64)short_side *
					    ps_margin_percent(request->margin_x_percent),
					    100U));
	margin_y = ps_max_u32(PS_MARGIN_PX_MIN,
		(u32)ps_round_div_u64((u64)short_side *
					    ps_margin_percent(request->margin_y_percent),
					    100U));
	if (margin_x > request->width_px / 2U ||
	    margin_y > request->height_px / 2U)
		return -ERANGE;
	safe_width = request->width_px - 2U * margin_x;
	safe_height = request->height_px - 2U * margin_y;
	physical_trusted = ps_physical_dpi(request, &dpi_y_q16) == 0 ? 1U : 0U;
	if (!physical_trusted)
		dpi_y_q16 = ps_fallback_dpi(short_side) << 16;

	for (scale = PS_FONT_SCALE_MIN; scale <= PS_FONT_SCALE_MAX; scale++) {
		u32 glyph_width = PS_FONT_GLYPH_WIDTH * scale;
		u32 glyph_height = PS_FONT_GLYPH_HEIGHT * scale;
		u32 line_gap = ps_spacing_round(glyph_height, 8U, 1U);
		u32 row_pitch = glyph_height + line_gap;
		u32 section_gap = ps_spacing_round(glyph_height, 4U, 2U);
		u32 header_height = PS_HEADER_ROWS * row_pitch + section_gap;
		u32 columns;
		u32 body_rows;
		u32 mode;
		u32 mode_score;
		u32 glyph_um;
		u32 outside;
		u32 error;
		struct ps_qr_geometry qr;

		if (safe_height <= header_height)
			continue;
		columns = safe_width / glyph_width;
		body_rows = (safe_height - header_height) / row_pitch;
		if (columns >= PS_NORMAL_COLUMNS_MIN &&
		    body_rows >= PS_NORMAL_ROWS_MIN) {
			mode = PS_LAYOUT_NORMAL;
			mode_score = 0U;
		} else if (columns >= PS_COMPACT_COLUMNS_MIN &&
			   body_rows >= PS_COMPACT_ROWS_MIN) {
			mode = PS_LAYOUT_COMPACT;
			mode_score = 1U;
		} else {
			continue;
		}
		if (ps_qr_geometry_from_safe(safe_width, safe_height, scale, &qr))
			continue;
		glyph_um = ps_glyph_height_um(glyph_height, dpi_y_q16);
		outside = glyph_um < PS_PHYSICAL_GLYPH_MIN_UM ||
			  glyph_um > PS_PHYSICAL_GLYPH_MAX_UM ? 1U : 0U;
		error = glyph_um > PS_PHYSICAL_GLYPH_TARGET_UM ?
			glyph_um - PS_PHYSICAL_GLYPH_TARGET_UM :
			PS_PHYSICAL_GLYPH_TARGET_UM - glyph_um;
		if (!ps_candidate_better(outside, error, mode_score, scale,
					 best_outside, best_error,
					 best_mode_score, best_scale, have_best))
			continue;
		have_best = 1U;
		best_outside = outside;
		best_error = error;
		best_mode_score = mode_score;
		best_scale = scale;
		best_mode = mode;
		best_glyph_um = glyph_um;
		best_qr = qr;
	}
	if (!have_best)
		return -ERANGE;

	layout->eligible = 1U;
	layout->mode = best_mode;
	layout->physical_trusted = physical_trusted;
	layout->dpi_y_q16 = dpi_y_q16;
	layout->glyph_height_um = best_glyph_um;
	layout->scale = best_scale;
	layout->glyph_width_px = PS_FONT_GLYPH_WIDTH * best_scale;
	layout->glyph_height_px = PS_FONT_GLYPH_HEIGHT * best_scale;
	layout->line_gap_px = ps_spacing_round(layout->glyph_height_px, 8U, 1U);
	layout->row_pitch_px = layout->glyph_height_px + layout->line_gap_px;
	layout->section_gap_px =
		ps_spacing_round(layout->glyph_height_px, 4U, 2U);
	layout->margin_x_px = margin_x;
	layout->margin_y_px = margin_y;
	layout->safe_x = margin_x;
	layout->safe_y = margin_y;
	layout->safe_width_px = safe_width;
	layout->safe_height_px = safe_height;
	layout->columns = safe_width / layout->glyph_width_px;
	layout->header_height_px =
		PS_HEADER_ROWS * layout->row_pitch_px + layout->section_gap_px;
	layout->body_rows =
		(safe_height - layout->header_height_px) / layout->row_pitch_px;
	layout->footer_height_px = PS_QR_FOOTER_ROWS * layout->row_pitch_px;
	layout->qr = best_qr;
	layout->qr.slot_x += layout->safe_x;
	layout->qr.slot_y += layout->safe_y;
	layout->qr.square_x += layout->safe_x;
	layout->qr.square_y += layout->safe_y;
	layout->qr.raster_x += layout->safe_x;
	layout->qr.raster_y += layout->safe_y;
	layout->qr.footer_y += layout->safe_y;
	return 0;
}
