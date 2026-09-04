// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_core.h"

static int ps_draw_valid(const struct ps_draw_ops *ops)
{
	return ops && ops->fill_rect && ops->blit_mono;
}

static int ps_draw_ascii_bytes(const struct ps_layout *layout, u32 x, u32 y,
			       const u8 *text, u32 text_len, u32 max_columns,
			       u32 color, const struct ps_draw_ops *ops)
{
	u32 index;
	u32 columns = 0U;

	for (index = 0U; index < text_len && columns < max_columns; index++) {
		u8 ch = text[index];
		const u8 *glyph;
		int ret;

		if (!ch)
			break;
		if (ch < PS_FONT_FIRST || ch > PS_FONT_LAST)
			ch = '?';
		glyph = ps_font_glyph(ch);
		ret = ops->blit_mono(ops->ctx,
				x + columns * layout->glyph_width_px, y,
				glyph, PS_FONT_GLYPH_WIDTH,
				PS_FONT_GLYPH_HEIGHT, 1U, layout->scale,
				color);
		if (ret)
			return ret;
		columns++;
	}
	return 0;
}

static int ps_draw_ascii(const struct ps_layout *layout, u32 x, u32 y,
			 const char *text, u32 max_columns, u32 color,
			 const struct ps_draw_ops *ops)
{
	u32 len = 0U;

	if (!text)
		return -EINVAL;
	while (len < max_columns && text[len])
		len++;
	return ps_draw_ascii_bytes(layout, x, y, (const u8 *)text, len,
				   max_columns, color, ops);
}

static int ps_draw_text_token(const struct ps_layout *layout, u32 x, u32 y,
			      const u8 *raw, u32 raw_len, u32 raw_offset,
			      const struct ps_text_token *token,
			      const struct ps_draw_ops *ops, u32 *display_len)
{
	u32 codepoint;
	int ret;

	if (!layout || !raw || !token || !ops || !display_len)
		return -EINVAL;
	/* Do not feed the LF projection ("\\n") to the glyph blitter. */
	if (token->hard_break) {
		*display_len = 0U;
		return 0;
	}
	ret = ps_text_display_info(raw, raw_len, raw_offset, display_len,
				   &codepoint);
	if (ret)
		return ret;
	if (!*display_len)
		return 0;
	if (*display_len == 1U && codepoint) {
		const u8 *glyph = ps_font_glyph_codepoint(codepoint);

		return ops->blit_mono(ops->ctx, x, y, glyph,
				      PS_FONT_GLYPH_WIDTH, PS_FONT_GLYPH_HEIGHT,
				      1U, layout->scale, PS_COLOR_WHITE);
	}
	return ps_draw_ascii_bytes(layout, x, y, token->ascii,
				   token->ascii_len, token->ascii_len,
				   PS_COLOR_WHITE, ops);
}

static int ps_draw_hex_status(const struct ps_layout *layout, u32 x, u32 y,
			      u32 flags, const struct ps_draw_ops *ops)
{
	static const char hex[] = "0123456789ABCDEF";
	char line[11];
	u32 index;

	line[0] = 'R';
	line[1] = ' ';
	for (index = 0U; index < 8U; index++)
		line[2U + index] =
			hex[(flags >> ((7U - index) * 4U)) & 0x0fU];
	line[10] = '\0';
	return ps_draw_ascii(layout, x, y, line, layout->columns,
			     PS_COLOR_WHITE, ops);
}

int ps_render_begin(const struct ps_layout *layout,
		    const struct ps_draw_ops *ops)
{
	u32 width;
	u32 height;

	if (!layout || !layout->eligible || !ps_draw_valid(ops))
		return -EINVAL;
	width = layout->safe_x + layout->safe_width_px + layout->margin_x_px;
	height = layout->safe_y + layout->safe_height_px + layout->margin_y_px;
	return ops->fill_rect(ops->ctx, 0U, 0U, width, height, PS_COLOR_BLACK);
}

int ps_render_header(const struct ps_layout *layout, const char *status_lines,
		     u32 status_stride, u32 runtime_flags,
		     const char *runtime_label,
		     const struct ps_draw_ops *ops)
{
	u32 row = 0U;
	u32 y;
	int ret;

	if (!layout || !layout->eligible || !status_lines ||
	    status_stride < 12U || !ps_draw_valid(ops))
		return -EINVAL;
	ret = ops->fill_rect(ops->ctx, layout->safe_x, layout->safe_y,
			     layout->safe_width_px, layout->header_height_px,
			     PS_COLOR_BLACK);
	if (ret)
		return ret;
	y = layout->safe_y;
	for (row = 0U; row < PS_TITLE_ROWS; row++) {
		ret = ps_draw_ascii(layout, layout->safe_x, y,
				    ps_title_line(row), layout->columns,
				    PS_COLOR_YELLOW, ops);
		if (ret)
			return ret;
		y += layout->row_pitch_px;
	}
	for (row = 0U; row < PS_HEADER_PSLC_STATUS_ROWS; row++) {
		ret = ps_draw_ascii_bytes(layout, layout->safe_x, y,
					  (const u8 *)status_lines +
						  row * status_stride,
					  status_stride, layout->columns,
					  PS_COLOR_WHITE, ops);
		if (ret)
			return ret;
		y += layout->row_pitch_px;
	}
	ret = ps_draw_hex_status(layout, layout->safe_x, y, runtime_flags, ops);
	if (ret)
		return ret;
	y += layout->row_pitch_px;
	return ps_draw_ascii(layout, layout->safe_x, y,
			     runtime_label ? runtime_label : "OK", layout->columns,
			     PS_COLOR_WHITE, ops);
}

static int ps_draw_text_row(const struct ps_layout *layout,
			    const struct ps_text_index *index,
			    const struct ps_text_row *row, u32 y,
			    const struct ps_draw_ops *ops)
{
	u32 raw_offset = row->raw_start;
	u32 column = 0U;

	while (raw_offset < row->raw_end) {
		struct ps_text_token token;
		u32 display_len;
		u32 codepoint;
		int ret;

		if (ps_text_next_token(index->raw, index->raw_len, raw_offset,
				       &token) ||
		    token.raw_len > row->raw_end - raw_offset ||
		    ps_text_display_info(index->raw, index->raw_len, raw_offset,
					 &display_len, &codepoint) ||
		    display_len > layout->columns - column)
			return -EINVAL;
		if (token.hard_break) {
			raw_offset += token.raw_len;
			continue;
		}
		(void)codepoint;
		ret = ps_draw_text_token(layout,
					 layout->safe_x +
						 column * layout->glyph_width_px,
					 y, index->raw, index->raw_len, raw_offset,
					 &token, ops, &display_len);
		if (ret)
			return ret;
		column += display_len;
		raw_offset += token.raw_len;
	}
	return 0;
}

int ps_render_text_page(const struct ps_layout *layout,
			const struct ps_text_index *index, u32 top_line,
			const struct ps_draw_ops *ops)
{
	u32 body_y;
	u32 row_index;
	u32 visible;
	int ret;

	if (!layout || !layout->eligible || !index || !index->columns ||
	    index->columns != layout->columns || !ps_draw_valid(ops))
		return -EINVAL;
	body_y = layout->safe_y + layout->header_height_px;
	ret = ops->fill_rect(ops->ctx, layout->safe_x, body_y,
			     layout->safe_width_px,
			     layout->safe_height_px - layout->header_height_px,
			     PS_COLOR_BLACK);
	if (ret)
		return ret;
	top_line = ps_text_scroll(top_line, index->total_rows,
				  layout->body_rows, 0);
	visible = index->total_rows - top_line;
	if (visible > layout->body_rows)
		visible = layout->body_rows;
	for (row_index = 0U; row_index < visible; row_index++) {
		struct ps_text_row row;

		ret = ps_text_row_get(index, top_line + row_index, &row);
		if (ret)
			return ret;
		ret = ps_draw_text_row(layout, index, &row,
				       body_y + row_index * layout->row_pitch_px,
				       ops);
		if (ret)
			return ret;
	}
	return 0;
}

int ps_render_qr_page(const struct ps_layout *layout, const u8 *matrix,
		      u32 matrix_size, const char *footer_lines,
		      u32 footer_stride, const struct ps_draw_ops *ops)
{
	u32 expected_size;
	u32 body_y;
	u32 row;
	int ret;

	if (!layout || !layout->eligible || !layout->qr.eligible || !matrix ||
	    !footer_lines || footer_stride < PS_QR_FOOTER_COLUMNS + 1U ||
	    !ps_draw_valid(ops))
		return -EINVAL;
	expected_size = 17U + 4U * layout->qr.version;
	if (matrix_size != expected_size ||
	    layout->qr.raster_side !=
		(matrix_size + 2U * PS_QR_QUIET_ZONE_MODULES) *
		layout->qr.module_px)
		return -EINVAL;
	body_y = layout->safe_y + layout->header_height_px;
	ret = ops->fill_rect(ops->ctx, layout->safe_x, body_y,
			     layout->safe_width_px,
			     layout->safe_height_px - layout->header_height_px,
			     PS_COLOR_BLACK);
	if (ret)
		return ret;
	ret = ops->fill_rect(ops->ctx, layout->qr.raster_x,
			     layout->qr.raster_y, layout->qr.raster_side,
			     layout->qr.raster_side, PS_COLOR_WHITE);
	if (ret)
		return ret;
	for (row = 0U; row < matrix_size; row++) {
		u32 column = 0U;

		while (column < matrix_size) {
			u32 start;

			while (column < matrix_size &&
			       !ps_qr_matrix_get(matrix, matrix_size, column, row))
				column++;
			start = column;
			while (column < matrix_size &&
			       ps_qr_matrix_get(matrix, matrix_size, column, row))
				column++;
			if (start < column) {
				ret = ops->fill_rect(ops->ctx,
					layout->qr.raster_x +
						(PS_QR_QUIET_ZONE_MODULES + start) *
						layout->qr.module_px,
					layout->qr.raster_y +
						(PS_QR_QUIET_ZONE_MODULES + row) *
						layout->qr.module_px,
					(column - start) * layout->qr.module_px,
					layout->qr.module_px, PS_COLOR_BLACK);
				if (ret)
					return ret;
			}
		}
	}
	for (row = 0U; row < PS_QR_FOOTER_ROWS; row++) {
		ret = ps_draw_ascii_bytes(layout, layout->safe_x,
			layout->qr.footer_y + row * layout->row_pitch_px,
			(const u8 *)footer_lines + row * footer_stride,
			footer_stride, PS_QR_FOOTER_COLUMNS,
			PS_COLOR_WHITE, ops);
		if (ret)
			return ret;
	}
	return 0;
}
