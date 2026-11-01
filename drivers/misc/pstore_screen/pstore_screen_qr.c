// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_core.h"
#include "qrcodegen.h"

static void ps_qr_put_le16(u8 *out, u16 value)
{
	out[0] = (u8)value;
	out[1] = (u8)(value >> 8);
}

static void ps_qr_put_le32(u8 *out, u32 value)
{
	out[0] = (u8)value;
	out[1] = (u8)(value >> 8);
	out[2] = (u8)(value >> 16);
	out[3] = (u8)(value >> 24);
}

static u32 ps_qr_expected_chunks(u32 total_len, u32 payload_capacity)
{
	if (!total_len || !payload_capacity)
		return 0U;
	return 1U + (total_len - 1U) / payload_capacity;
}

int ps_qr_set_prepare(struct ps_qr_set *set, const u8 *log, u32 total_len,
		      u32 pslc_container_flags,
		      const struct ps_qr_geometry *geometry)
{
	u8 digest[32];
	u32 chunks;

	if (!set || !log || !geometry || !geometry->eligible ||
	    geometry->version < PS_QR_MIN_VERSION ||
	    geometry->version > PS_QR_MAX_VERSION ||
	    geometry->module_px < PS_QR_MIN_MODULE_PX ||
	    geometry->frame_capacity <= PSQ1_HEADER_LEN ||
	    geometry->payload_capacity !=
		geometry->frame_capacity - PSQ1_HEADER_LEN ||
	    total_len < PSLC_MIN_TOTAL_LEN ||
	    (pslc_container_flags & ~PSLC_CONTAINER_FLAGS_ALLOWED_MASK))
		return -EINVAL;
	chunks = ps_qr_expected_chunks(total_len, geometry->payload_capacity);
	if (!chunks)
		return -EOVERFLOW;
	memset(set, 0, sizeof(*set));
	set->log = log;
	set->total_len = total_len;
	set->payload_capacity = geometry->payload_capacity;
	set->chunk_count = chunks;
	set->whole_crc32 = ps_crc32(log, total_len);
	set->container_truncated =
		(pslc_container_flags & PSLC_CONTAINER_FLAG_TRUNCATED) ? 1U : 0U;
	set->version = geometry->version;
	set->module_px = geometry->module_px;
	ps_sha256(log, total_len, digest);
	memcpy(set->log_id, digest, sizeof(set->log_id));
	memset(digest, 0, sizeof(digest));
	return 0;
}

int ps_qr_build_frame(const struct ps_qr_set *set, u32 chunk_index, u8 *out,
		      u32 out_capacity, u32 *out_len,
		      struct ps_qr_frame_info *info)
{
	u64 offset64;
	u32 offset;
	u32 payload_len;
	u32 frame_len;
	u32 flags;
	u32 state;

	if (!set || !set->log || !out || !out_len || !info ||
	    !set->payload_capacity || !set->chunk_count ||
	    set->total_len < PSLC_MIN_TOTAL_LEN ||
	    set->chunk_count !=
		ps_qr_expected_chunks(set->total_len, set->payload_capacity) ||
	    chunk_index >= set->chunk_count)
		return -EINVAL;
	offset64 = (u64)chunk_index * set->payload_capacity;
	if (offset64 > 0xffffffffU || offset64 >= set->total_len)
		return -EOVERFLOW;
	offset = (u32)offset64;
	payload_len = set->total_len - offset;
	if (payload_len > set->payload_capacity)
		payload_len = set->payload_capacity;
	if (payload_len > 0xffffffffU - PSQ1_HEADER_LEN)
		return -EOVERFLOW;
	frame_len = PSQ1_HEADER_LEN + payload_len;
	if (out_capacity < frame_len)
		return -ENOSPC;

	flags = set->container_truncated ?
		PSQ1_FLAG_CONTAINER_TRUNCATED : 0U;
	if (!chunk_index)
		flags |= PSQ1_FLAG_FIRST;
	if (chunk_index + 1U == set->chunk_count)
		flags |= PSQ1_FLAG_LAST;
	memset(out, 0, frame_len);
	out[0] = 'P';
	out[1] = 'S';
	out[2] = 'Q';
	out[3] = '1';
	out[4] = 1U;
	out[5] = (u8)flags;
	ps_qr_put_le16(out + 6U, PSQ1_HEADER_LEN);
	memcpy(out + 8U, set->log_id, sizeof(set->log_id));
	ps_qr_put_le32(out + 16U, chunk_index);
	ps_qr_put_le32(out + 20U, set->chunk_count);
	ps_qr_put_le32(out + 24U, offset);
	ps_qr_put_le32(out + 28U, payload_len);
	ps_qr_put_le32(out + 32U, set->total_len);
	ps_qr_put_le32(out + 36U, set->whole_crc32);
	memcpy(out + PSQ1_HEADER_LEN, set->log + offset, payload_len);
	state = ps_crc32_feed(ps_crc32_begin(), out, 40U);
	state = ps_crc32_feed(state, out + PSQ1_HEADER_LEN, payload_len);
	state = ps_crc32_end(state);
	ps_qr_put_le32(out + 40U, state);

	memset(info, 0, sizeof(*info));
	info->chunk_index = chunk_index;
	info->chunk_count = set->chunk_count;
	info->payload_offset = offset;
	info->payload_len = payload_len;
	info->chunk_crc32 = state;
	info->flags = flags;
	*out_len = frame_len;
	return 0;
}

static void ps_qr_hex8(char *out, u32 value)
{
	static const char hex[] = "0123456789ABCDEF";
	u32 index;

	for (index = 0U; index < 8U; index++)
		out[index] = hex[(value >> ((7U - index) * 4U)) & 0x0fU];
}

static void ps_qr_footer_clear(char *line, u32 stride)
{
	memset(line, 0, stride);
}

int ps_qr_format_footer(const struct ps_qr_frame_info *info, char *out,
			u32 stride)
{
	char *line;

	if (!info || !out || stride < PS_QR_FOOTER_COLUMNS + 1U ||
	    !info->chunk_count || info->chunk_index >= info->chunk_count)
		return -EINVAL;
	line = out;
	ps_qr_footer_clear(line, stride);
	line[0] = 'Q';
	line[1] = 'R';
	line[2] = ' ';
	ps_qr_hex8(line + 3U, info->chunk_index + 1U);
	line[11] = '/';
	ps_qr_hex8(line + 12U, info->chunk_count);
	line = out + stride;
	ps_qr_footer_clear(line, stride);
	line[0] = 'O';
	ps_qr_hex8(line + 1U, info->payload_offset);
	line[9] = ' ';
	line[10] = 'L';
	ps_qr_hex8(line + 11U, info->payload_len);
	line = out + 2U * stride;
	ps_qr_footer_clear(line, stride);
	line[0] = 'C';
	ps_qr_hex8(line + 1U, info->chunk_crc32);
	return 0;
}

u32 ps_qr_page_move(u32 current_page, u32 count, s32 delta)
{
	u32 maximum;

	if (!count)
		return 0U;
	maximum = count - 1U;
	if (current_page > maximum)
		current_page = maximum;
	if (delta >= 0) {
		u32 amount = (u32)delta;

		if (amount > maximum - current_page)
			return maximum;
		return current_page + amount;
	}
	{
		u32 amount = (u32)(-(s64)delta);

		return amount > current_page ? 0U : current_page - amount;
	}
}

u32 ps_qr_workbuf_len(u32 version)
{
	if (version < PS_QR_MIN_VERSION || version > PS_QR_MAX_VERSION)
		return 0U;
	return (u32)qrcodegen_BUFFER_LEN_FOR_VERSION(version);
}

u32 ps_qr_matrix_len(u32 version)
{
	u32 size;
	u32 pitch;

	if (version < PS_QR_MIN_VERSION || version > PS_QR_MAX_VERSION)
		return 0U;
	size = 17U + 4U * version;
	pitch = (size + 7U) / 8U;
	return pitch * size;
}

int ps_qr_encode_matrix(const u8 *frame, u32 frame_len, u32 version,
			u8 *temp, u32 temp_len, u8 *qr_work,
			u32 qr_work_len, u8 *matrix, u32 matrix_len,
			u32 *matrix_size)
{
	u32 required_work;
	u32 required_matrix;
	u32 size;
	u32 pitch;
	u32 y;

	if (!frame || !frame_len || !temp || !qr_work || !matrix ||
	    !matrix_size)
		return -EINVAL;
	required_work = ps_qr_workbuf_len(version);
	required_matrix = ps_qr_matrix_len(version);
	if (!required_work || !required_matrix || temp_len < required_work ||
	    qr_work_len < required_work || matrix_len < required_matrix ||
	    frame_len > temp_len)
		return -ENOSPC;
	memcpy(temp, frame, frame_len);
	if (!qrcodegen_encodeBinary(temp, (size_t)frame_len, qr_work,
				    qrcodegen_Ecc_LOW, (int)version,
				    (int)version, qrcodegen_Mask_AUTO, false))
		return -ERANGE;
	size = (u32)qrcodegen_getSize(qr_work);
	if (size != 17U + 4U * version)
		return -EINVAL;
	pitch = (size + 7U) / 8U;
	memset(matrix, 0, required_matrix);
	for (y = 0U; y < size; y++) {
		u32 x;

		for (x = 0U; x < size; x++) {
			if (qrcodegen_getModule(qr_work, (int)x, (int)y))
				matrix[y * pitch + x / 8U] |=
					(u8)(0x80U >> (x & 7U));
		}
	}
	*matrix_size = size;
	return 0;
}

int ps_qr_matrix_get(const u8 *matrix, u32 matrix_size, u32 x, u32 y)
{
	u32 pitch;

	if (!matrix || matrix_size < 21U || matrix_size > 177U ||
	    x >= matrix_size || y >= matrix_size)
		return 0;
	pitch = (matrix_size + 7U) / 8U;
	return (matrix[y * pitch + x / 8U] &
		(u8)(0x80U >> (x & 7U))) != 0U;
}
