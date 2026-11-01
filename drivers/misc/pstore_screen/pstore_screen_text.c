// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_core.h"

static const u8 ps_hex[] = "0123456789ABCDEF";

static void ps_token_byte_escape(u8 value, struct ps_text_token *token)
{
	token->ascii[0] = '\\';
	token->ascii[1] = 'x';
	token->ascii[2] = ps_hex[value >> 4];
	token->ascii[3] = ps_hex[value & 0x0fU];
	token->ascii_len = 4U;
	token->raw_len = 1U;
	token->hard_break = 0U;
}

static int ps_utf8_cont(u8 value)
{
	return value >= 0x80U && value <= 0xbfU;
}

static int ps_utf8_decode(const u8 *raw, u32 remaining, u32 *codepoint,
			  u32 *consumed)
{
	u8 b0;
	u32 cp;
	u32 need;
	u32 i;

	if (!raw || !remaining || !codepoint || !consumed)
		return -EINVAL;
	b0 = raw[0];
	if (b0 >= 0xc2U && b0 <= 0xdfU) {
		need = 2U;
		cp = (u32)(b0 & 0x1fU);
	} else if (b0 >= 0xe0U && b0 <= 0xefU) {
		need = 3U;
		cp = (u32)(b0 & 0x0fU);
	} else if (b0 >= 0xf0U && b0 <= 0xf4U) {
		need = 4U;
		cp = (u32)(b0 & 0x07U);
	} else {
		return -EINVAL;
	}
	if (remaining < need)
		return -EINVAL;
	for (i = 1U; i < need; i++) {
		if (!ps_utf8_cont(raw[i]))
			return -EINVAL;
	}
	if ((b0 == 0xe0U && raw[1] < 0xa0U) ||
	    (b0 == 0xedU && raw[1] > 0x9fU) ||
	    (b0 == 0xf0U && raw[1] < 0x90U) ||
	    (b0 == 0xf4U && raw[1] > 0x8fU))
		return -EINVAL;
	for (i = 1U; i < need; i++)
		cp = (cp << 6) | (u32)(raw[i] & 0x3fU);
	if (cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU))
		return -EINVAL;
	*codepoint = cp;
	*consumed = need;
	return 0;
}

static void ps_token_codepoint(u32 codepoint, u32 consumed,
			       struct ps_text_token *token)
{
	u32 digits = 4U;
	u32 value = codepoint;
	u32 i;

	if (codepoint > 0xfffffU)
		digits = 6U;
	else if (codepoint > 0xffffU)
		digits = 5U;
	token->ascii[0] = '\\';
	token->ascii[1] = 'u';
	token->ascii[2] = '{';
	for (i = 0U; i < digits; i++) {
		token->ascii[2U + digits - i] = ps_hex[value & 0x0fU];
		value >>= 4;
	}
	token->ascii[3U + digits] = '}';
	token->ascii_len = digits + 4U;
	token->raw_len = consumed;
	token->hard_break = 0U;
}

int ps_text_next_token(const u8 *raw, u32 raw_len, u32 raw_offset,
		       struct ps_text_token *token)
{
	u8 value;
	u32 codepoint;
	u32 consumed;

	if (!raw || !token || raw_offset >= raw_len)
		return -EINVAL;
	memset(token, 0, sizeof(*token));
	value = raw[raw_offset];
	if (value >= 0x20U && value <= 0x7eU) {
		if (value == '\\') {
			token->ascii[0] = '\\';
			token->ascii[1] = '\\';
			token->ascii_len = 2U;
		} else {
			token->ascii[0] = value;
			token->ascii_len = 1U;
		}
		token->raw_len = 1U;
		return 0;
	}
	if (value == '\r' || value == '\n' || value == '\t') {
		token->ascii[0] = '\\';
		token->ascii[1] = value == '\r' ? 'r' :
				  (value == '\n' ? 'n' : 't');
		token->ascii_len = 2U;
		token->raw_len = 1U;
		token->hard_break = value == '\n' ? 1U : 0U;
		return 0;
	}
	if (value < 0x80U) {
		ps_token_byte_escape(value, token);
		return 0;
	}
	if (ps_utf8_decode(raw + raw_offset, raw_len - raw_offset,
			   &codepoint, &consumed)) {
		ps_token_byte_escape(value, token);
		return 0;
	}
	ps_token_codepoint(codepoint, consumed, token);
	return 0;
}

int ps_text_display_info(const u8 *raw, u32 raw_len, u32 raw_offset,
			 u32 *display_len, u32 *codepoint)
{
	struct ps_text_token token;
	u32 decoded;
	u32 consumed;
	u8 value;

	if (!raw || !display_len || !codepoint || raw_offset >= raw_len)
		return -EINVAL;
	if (ps_text_next_token(raw, raw_len, raw_offset, &token))
		return -EINVAL;
	/* A hard break is layout control, never printable screen text. */
	*display_len = token.hard_break ? 0U : token.ascii_len;
	*codepoint = 0U;
	value = raw[raw_offset];
	if (token.hard_break)
		return 0;
	if (value >= PS_FONT_FIRST && value <= PS_FONT_LAST) {
		*display_len = 1U;
		*codepoint = (u32)value;
		return 0;
	}
	if (value >= 0x80U &&
	    !ps_utf8_decode(raw + raw_offset, raw_len - raw_offset,
			    &decoded, &consumed)) {
		*display_len = 1U;
		*codepoint = decoded;
	}
	return 0;
}

int ps_text_project(const u8 *raw, u32 raw_len, char *out, u32 out_capacity,
		    u32 *out_len)
{
	u32 raw_offset = 0U;
	u32 used = 0U;

	if ((!raw && raw_len) || !out || !out_len || !out_capacity)
		return -EINVAL;
	while (raw_offset < raw_len) {
		struct ps_text_token token;

		if (ps_text_next_token(raw, raw_len, raw_offset, &token))
			return -EINVAL;
		if (token.ascii_len >= out_capacity - used) {
			*out_len = used;
			return -ENOSPC;
		}
		memcpy(out + used, token.ascii, token.ascii_len);
		used += token.ascii_len;
		raw_offset += token.raw_len;
	}
	out[used] = '\0';
	*out_len = used;
	return 0;
}

static void ps_store_row(struct ps_text_index *index, u32 raw_start,
			 u32 raw_end, u32 columns, u32 hard_break)
{
	if (index->total_rows < index->row_capacity) {
		struct ps_text_row *row = &index->rows[index->total_rows];

		row->raw_start = raw_start;
		row->raw_end = raw_end;
		row->columns = columns;
		row->hard_break = hard_break;
		index->indexed_rows++;
	}
	index->total_rows++;
}

int ps_text_index_build(struct ps_text_index *index, const u8 *raw, u32 raw_len,
			u32 columns, struct ps_text_row *rows,
			u32 row_capacity)
{
	u32 raw_offset = 0U;
	u32 row_start = 0U;
	u32 used_columns = 0U;
	u32 ended_with_lf = 0U;

	if (!index || (!raw && raw_len) || !columns || (!rows && row_capacity))
		return -EINVAL;
	memset(index, 0, sizeof(*index));
	index->raw = raw;
	index->raw_len = raw_len;
	index->columns = columns;
	index->rows = rows;
	index->row_capacity = row_capacity;

	while (raw_offset < raw_len) {
		struct ps_text_token token;
		u32 display_len;
		u32 codepoint;

		if (ps_text_next_token(raw, raw_len, raw_offset, &token))
			return -EINVAL;
		if (ps_text_display_info(raw, raw_len, raw_offset, &display_len,
					 &codepoint))
			return -EINVAL;
		(void)codepoint;
		if (display_len > columns)
			return -ERANGE;
		if (used_columns && display_len > columns - used_columns) {
			ps_store_row(index, row_start, raw_offset, used_columns, 0U);
			index->soft_wraps++;
			row_start = raw_offset;
			used_columns = 0U;
		}
		used_columns += display_len;
		raw_offset += token.raw_len;
		ended_with_lf = token.hard_break;
		if (token.hard_break) {
			ps_store_row(index, row_start, raw_offset, used_columns, 1U);
			index->hard_breaks++;
			row_start = raw_offset;
			used_columns = 0U;
		}
	}
	if (!raw_len || row_start < raw_len || ended_with_lf)
		ps_store_row(index, row_start, raw_len, used_columns, 0U);
	index->complete = index->indexed_rows == index->total_rows ? 1U : 0U;
	return 0;
}

static int ps_text_find_row(const struct ps_text_index *index, u32 wanted,
			    struct ps_text_row *result)
{
	u32 raw_offset = 0U;
	u32 row_start = 0U;
	u32 used_columns = 0U;
	u32 row_index = 0U;
	u32 ended_with_lf = 0U;

	while (raw_offset < index->raw_len) {
		struct ps_text_token token;
		u32 display_len;
		u32 codepoint;

		if (ps_text_next_token(index->raw, index->raw_len, raw_offset,
				       &token))
			return -EINVAL;
		if (ps_text_display_info(index->raw, index->raw_len, raw_offset,
					 &display_len, &codepoint))
			return -EINVAL;
		(void)codepoint;
		if (used_columns &&
		    display_len > index->columns - used_columns) {
			if (row_index == wanted) {
				result->raw_start = row_start;
				result->raw_end = raw_offset;
				result->columns = used_columns;
				result->hard_break = 0U;
				return 0;
			}
			row_index++;
			row_start = raw_offset;
			used_columns = 0U;
		}
		used_columns += display_len;
		raw_offset += token.raw_len;
		ended_with_lf = token.hard_break;
		if (token.hard_break) {
			if (row_index == wanted) {
				result->raw_start = row_start;
				result->raw_end = raw_offset;
				result->columns = used_columns;
				result->hard_break = 1U;
				return 0;
			}
			row_index++;
			row_start = raw_offset;
			used_columns = 0U;
		}
	}
	if ((!index->raw_len || row_start < index->raw_len || ended_with_lf) &&
	    row_index == wanted) {
		result->raw_start = row_start;
		result->raw_end = index->raw_len;
		result->columns = used_columns;
		result->hard_break = 0U;
		return 0;
	}
	return -ERANGE;
}

int ps_text_row_get(const struct ps_text_index *index, u32 row_index,
		    struct ps_text_row *row)
{
	if (!index || !row || row_index >= index->total_rows)
		return -EINVAL;
	if (row_index < index->indexed_rows) {
		*row = index->rows[row_index];
		return 0;
	}
	return ps_text_find_row(index, row_index, row);
}

int ps_text_row_project(const struct ps_text_index *index,
			const struct ps_text_row *row, char *out,
			u32 out_capacity, u32 *out_len)
{
	u32 raw_offset;
	u32 used = 0U;

	if (!index || !row || !out || !out_len || !out_capacity ||
	    row->raw_start > row->raw_end || row->raw_end > index->raw_len)
		return -EINVAL;
	raw_offset = row->raw_start;
	while (raw_offset < row->raw_end) {
		struct ps_text_token token;

		if (ps_text_next_token(index->raw, index->raw_len, raw_offset,
				       &token) ||
		    token.raw_len > row->raw_end - raw_offset)
			return -EINVAL;
		if (token.ascii_len >= out_capacity - used) {
			*out_len = used;
			return -ENOSPC;
		}
		memcpy(out + used, token.ascii, token.ascii_len);
		used += token.ascii_len;
		raw_offset += token.raw_len;
	}
	out[used] = '\0';
	*out_len = used;
	return 0;
}

u32 ps_text_scroll(u32 top_line, u32 total_rows, u32 visible_rows, s32 delta)
{
	u32 maximum;

	if (!visible_rows || total_rows <= visible_rows)
		return 0U;
	maximum = total_rows - visible_rows;
	if (top_line > maximum)
		top_line = maximum;
	if (delta >= 0) {
		u32 amount = (u32)delta;

		if (amount > maximum - top_line)
			return maximum;
		return top_line + amount;
	}
	{
		u32 amount = (u32)(-(s64)delta);

		return amount > top_line ? 0U : top_line - amount;
	}
}
