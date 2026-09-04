// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_core.h"

static u16 ps_get_le16(const u8 *p)
{
	return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 ps_get_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static u64 ps_get_le64(const u8 *p)
{
	return (u64)ps_get_le32(p) | ((u64)ps_get_le32(p + 4) << 32);
}

static void ps_put_le16(u8 *p, u16 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
}

static void ps_put_le32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}

static void ps_put_le64(u8 *p, u64 value)
{
	ps_put_le32(p, (u32)value);
	ps_put_le32(p + 4, (u32)(value >> 32));
}

static int ps_add_u32(u32 a, u32 b, u32 *out)
{
	if (!out || b > 0xffffffffU - a)
		return -EOVERFLOW;
	*out = a + b;
	return 0;
}

static u32 ps_reason_flags(u32 events)
{
	u32 flags = 0U;

	if (events & PS_MIRROR_EVENT_OVERFLOW)
		flags |= PSLC_CONTAINER_FLAG_MIRROR_OVERFLOW;
	if (events & PS_MIRROR_EVENT_GENERATION_GAP)
		flags |= PSLC_CONTAINER_FLAG_MIRROR_GENERATION_GAP;
	if (events & PS_MIRROR_EVENT_COPY_FAILURE)
		flags |= PSLC_CONTAINER_FLAG_MIRROR_COPY_FAILURE;
	if (events & PS_MIRROR_EVENT_SCAN_INCOMPLETE)
		flags |= PSLC_CONTAINER_FLAG_MIRROR_SCAN_INCOMPLETE;
	if (events & PS_MIRROR_EVENT_BACKEND_READ_FAILURE)
		flags |= PSLC_CONTAINER_FLAG_MIRROR_BACKEND_READ_FAILURE;
	return flags;
}

static int ps_codec_valid(u32 codec)
{
	return codec <= PSLC_CODEC_ZSTD;
}

static int ps_pstore_type_valid(u32 type)
{
	return (type >= PSLC_PSTORE_TYPE_DMESG &&
		type <= PSLC_PSTORE_TYPE_PPC_OPAL) ||
	       type == PSLC_PSTORE_TYPE_UNKNOWN;
}

static int ps_pstore_ordered(const struct ps_pslc_segment_input *left,
			     const struct ps_pslc_segment_input *right)
{
	if (left->timestamp_sec != right->timestamp_sec)
		return left->timestamp_sec < right->timestamp_sec;
	if (left->timestamp_nsec != right->timestamp_nsec)
		return left->timestamp_nsec < right->timestamp_nsec;
	if (left->pstore_type != right->pstore_type)
		return left->pstore_type < right->pstore_type;
	if (left->record_id != right->record_id)
		return left->record_id < right->record_id;
	return left->ordinal <= right->ordinal;
}

static int ps_segment_semantics(const struct ps_pslc_segment_input *seg)
{
	u32 low;
	u32 codec;
	u32 compressed;
	u32 decompress_error;
	u32 raw_compressed;
	u32 has_ecc;

	if (!seg || (!seg->payload && seg->payload_len) ||
	    (!seg->ecc && seg->ecc_len) || seg->timestamp_nsec >= 1000000000U)
		return -EINVAL;
	if (seg->flags & ~(PSLC_SEGMENT_FLAGS_LOW_ALLOWED_MASK |
			   PSLC_SEGMENT_CODEC_MASK))
		return -EINVAL;
	low = seg->flags & PSLC_SEGMENT_FLAGS_LOW_ALLOWED_MASK;
	codec = (seg->flags & PSLC_SEGMENT_CODEC_MASK) >>
		PSLC_SEGMENT_CODEC_SHIFT;
	if (!ps_codec_valid(codec))
		return -EINVAL;

	switch (seg->kind) {
	case PSLC_SEGMENT_KIND_PANIC_DESCRIPTION:
		if (seg->flags || seg->ordinal || seg->pstore_type ||
		    seg->record_id || seg->timestamp_sec ||
		    seg->timestamp_nsec || seg->ecc_len)
			return -EINVAL;
		return 0;
	case PSLC_SEGMENT_KIND_CURRENT_KMSG:
		if (low & ~PSLC_SEGMENT_FLAG_TRUNCATED || codec ||
		    seg->ordinal || seg->pstore_type || seg->record_id ||
		    seg->timestamp_sec || seg->timestamp_nsec || seg->ecc_len)
			return -EINVAL;
		return 0;
	case PSLC_SEGMENT_KIND_PSTORE_RECORD:
		if ((low & PSLC_SEGMENT_FLAG_TRUNCATED) ||
		    !ps_pstore_type_valid(seg->pstore_type))
			return -EINVAL;
		compressed = (low & PSLC_SEGMENT_FLAG_COMPRESSED) ? 1U : 0U;
		decompress_error =
			(low & PSLC_SEGMENT_FLAG_DECOMPRESS_ERROR) ? 1U : 0U;
		raw_compressed =
			(low & PSLC_SEGMENT_FLAG_PAYLOAD_IS_RAW_COMPRESSED) ? 1U : 0U;
		has_ecc = (low & PSLC_SEGMENT_FLAG_HAS_ECC) ? 1U : 0U;
		if ((compressed != (codec != PSLC_CODEC_NONE)) ||
		    (has_ecc != (seg->ecc_len != 0U)) ||
		    (decompress_error != raw_compressed) ||
		    (decompress_error && !compressed))
			return -EINVAL;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ps_expected_container_flags(const struct ps_pslc_build_input *input,
				       u32 current_payload_len,
				       u32 serialized_pstore_count,
				       u32 *expected)
{
	u32 flags = 0U;
	u32 reasons;
	u32 current_truncated;
	u32 dropped;

	if (!input || !expected ||
	    (input->mirror_events & ~PS_MIRROR_EVENTS_ALLOWED_MASK) ||
	    !input->tail_capacity || input->current_total_len < current_payload_len ||
	    input->pstore_total_count < serialized_pstore_count)
		return -EINVAL;
	if ((input->current_total_len == 0U) != (current_payload_len == 0U))
		return -EINVAL;
	if (current_payload_len !=
	    (input->current_total_len < input->tail_capacity ?
	     input->current_total_len : input->tail_capacity))
		return -EINVAL;
	current_truncated = input->current_total_len > current_payload_len ? 1U : 0U;
	dropped = input->pstore_total_count > serialized_pstore_count ? 1U : 0U;
	reasons = ps_reason_flags(input->mirror_events);

	if (current_truncated)
		flags |= PSLC_CONTAINER_FLAG_TRUNCATED;
	if (!input->current_total_len)
		flags |= PSLC_CONTAINER_FLAG_CURRENT_KMSG_EMPTY;
	if (dropped)
		flags |= PSLC_CONTAINER_FLAG_PSTORE_DROPPED |
			 PSLC_CONTAINER_FLAG_TRUNCATED;

	switch (input->mirror_source) {
	case PS_MIRROR_SOURCE_ABSENT:
		if (input->mirror_events || input->pstore_total_count ||
		    serialized_pstore_count)
			return -EINVAL;
		flags |= PSLC_CONTAINER_FLAG_RAMOOPS_UNAVAILABLE;
		break;
	case PS_MIRROR_SOURCE_CERTIFIED_COMPLETE:
		if (input->mirror_events)
			return -EINVAL;
		break;
	case PS_MIRROR_SOURCE_CERTIFIED_INCOMPLETE:
		if (!input->mirror_events)
			return -EINVAL;
		flags |= reasons | PSLC_CONTAINER_FLAG_MIRROR_INCOMPLETE |
			 PSLC_CONTAINER_FLAG_TRUNCATED;
		break;
	default:
		return -EINVAL;
	}
	*expected = flags;
	return 0;
}

static int ps_validate_build_input(const struct ps_pslc_build_input *input,
				   u32 *measured)
{
	u32 total = PSLC_HEADER_LEN;
	u32 current_payload_len = 0U;
	u32 pstore_count = 0U;
	u32 expected_flags;
	u32 i;

	if (!input || !measured || !input->segments || input->segment_count < 2U)
		return -EINVAL;
	if (input->segments[0].kind != PSLC_SEGMENT_KIND_PANIC_DESCRIPTION ||
	    input->segments[1].kind != PSLC_SEGMENT_KIND_CURRENT_KMSG)
		return -EINVAL;
	for (i = 0U; i < input->segment_count; i++) {
		const struct ps_pslc_segment_input *seg = &input->segments[i];
		u32 part;
		u32 padding;

		if (ps_segment_semantics(seg))
			return -EINVAL;
		if (i > 1U && seg->kind != PSLC_SEGMENT_KIND_PSTORE_RECORD)
			return -EINVAL;
		if (i > 2U && !ps_pstore_ordered(&input->segments[i - 1U], seg))
			return -EINVAL;
		if (seg->kind == PSLC_SEGMENT_KIND_CURRENT_KMSG)
			current_payload_len = seg->payload_len;
		if (seg->kind == PSLC_SEGMENT_KIND_PSTORE_RECORD)
			pstore_count++;
		if (ps_add_u32(PSLC_SEGMENT_HEADER_LEN, seg->payload_len, &part) ||
		    ps_add_u32(part, seg->ecc_len, &part))
			return -EOVERFLOW;
		padding = (4U - (part & 3U)) & 3U;
		if (ps_add_u32(part, padding, &part) ||
		    ps_add_u32(total, part, &total))
			return -EOVERFLOW;
	}
	if (ps_expected_container_flags(input, current_payload_len, pstore_count,
					&expected_flags) ||
	    expected_flags != input->container_flags)
		return -EINVAL;
	if (((input->segments[1].flags & PSLC_SEGMENT_FLAG_TRUNCATED) != 0U) !=
	    (input->current_total_len > current_payload_len))
		return -EINVAL;
	if (total < PSLC_MIN_TOTAL_LEN)
		return -EINVAL;
	*measured = total;
	return 0;
}

int ps_pslc_measure(const struct ps_pslc_build_input *input, u32 *total_len)
{
	return ps_validate_build_input(input, total_len);
}

int ps_pslc_build(const struct ps_pslc_build_input *input, u8 *out,
		  u32 out_capacity, u32 *out_len)
{
	u32 total;
	u32 offset = PSLC_HEADER_LEN;
	u32 i;

	if (!out || !out_len)
		return -EINVAL;
	if (ps_validate_build_input(input, &total))
		return -EINVAL;
	if (out_capacity < total)
		return -ENOSPC;
	memset(out, 0, total);
	out[0] = 'P';
	out[1] = 'S';
	out[2] = 'L';
	out[3] = 'C';
	ps_put_le16(out + 4, (u16)PSLC_VERSION);
	ps_put_le16(out + 6, (u16)PSLC_HEADER_LEN);
	ps_put_le32(out + 8, total);
	ps_put_le32(out + 12, input->segment_count);
	ps_put_le32(out + 16, input->container_flags);
	ps_put_le32(out + 20, input->current_total_len);
	ps_put_le32(out + 24, input->pstore_total_count);
	ps_put_le32(out + 28, ps_crc32(out, 28U));

	for (i = 0U; i < input->segment_count; i++) {
		const struct ps_pslc_segment_input *seg = &input->segments[i];
		u32 state;
		u32 data_len = seg->payload_len + seg->ecc_len;
		u32 padding = (4U - (data_len & 3U)) & 3U;

		ps_put_le16(out + offset, (u16)seg->kind);
		ps_put_le16(out + offset + 2U, (u16)seg->flags);
		ps_put_le16(out + offset + 4U, (u16)PSLC_SEGMENT_HEADER_LEN);
		ps_put_le16(out + offset + 6U, 0U);
		ps_put_le32(out + offset + 8U, seg->ordinal);
		ps_put_le32(out + offset + 12U, seg->pstore_type);
		ps_put_le64(out + offset + 16U, seg->record_id);
		ps_put_le64(out + offset + 24U, seg->timestamp_sec);
		ps_put_le32(out + offset + 32U, seg->timestamp_nsec);
		ps_put_le32(out + offset + 36U, seg->payload_len);
		ps_put_le32(out + offset + 40U, seg->ecc_len);
		state = ps_crc32_feed(ps_crc32_begin(), seg->payload,
				      seg->payload_len);
		state = ps_crc32_feed(state, seg->ecc, seg->ecc_len);
		ps_put_le32(out + offset + 44U, ps_crc32_end(state));
		offset += PSLC_SEGMENT_HEADER_LEN;
		if (seg->payload_len)
			memcpy(out + offset, seg->payload, seg->payload_len);
		offset += seg->payload_len;
		if (seg->ecc_len)
			memcpy(out + offset, seg->ecc, seg->ecc_len);
		offset += seg->ecc_len + padding;
	}
	if (offset != total)
		return -EINVAL;
	*out_len = total;
	return 0;
}

struct ps_parsed_pstore_order {
	u64 timestamp_sec;
	u64 record_id;
	u32 timestamp_nsec;
	u32 pstore_type;
	u32 ordinal;
	u32 valid;
};

static int ps_parsed_ordered(const struct ps_parsed_pstore_order *left,
			     const struct ps_parsed_pstore_order *right)
{
	if (!left->valid)
		return 1;
	if (left->timestamp_sec != right->timestamp_sec)
		return left->timestamp_sec < right->timestamp_sec;
	if (left->timestamp_nsec != right->timestamp_nsec)
		return left->timestamp_nsec < right->timestamp_nsec;
	if (left->pstore_type != right->pstore_type)
		return left->pstore_type < right->pstore_type;
	if (left->record_id != right->record_id)
		return left->record_id < right->record_id;
	return left->ordinal <= right->ordinal;
}

int ps_pslc_validate(const u8 *data, u32 data_len,
		     const struct ps_pslc_validation_context *context,
		     struct ps_pslc_summary *summary)
{
	struct ps_parsed_pstore_order previous = { 0 };
	u32 total_len;
	u32 segment_count;
	u32 container_flags;
	u32 current_total_len;
	u32 pstore_total_count;
	u32 current_payload_len = 0U;
	u32 current_flags = 0U;
	u32 pstore_count = 0U;
	u32 offset = PSLC_HEADER_LEN;
	u32 i;
	u32 expected_flags;
	struct ps_pslc_build_input synthetic;

	if (!data || !context || !summary || data_len < PSLC_MIN_TOTAL_LEN ||
	    data[0] != 'P' || data[1] != 'S' || data[2] != 'L' || data[3] != 'C' ||
	    ps_get_le16(data + 4) != PSLC_VERSION ||
	    ps_get_le16(data + 6) != PSLC_HEADER_LEN ||
	    ps_get_le32(data + 28) != ps_crc32(data, 28U))
		return -EINVAL;
	total_len = ps_get_le32(data + 8);
	segment_count = ps_get_le32(data + 12);
	container_flags = ps_get_le32(data + 16);
	current_total_len = ps_get_le32(data + 20);
	pstore_total_count = ps_get_le32(data + 24);
	if (total_len != data_len || segment_count < 2U ||
	    (container_flags & ~PSLC_CONTAINER_FLAGS_ALLOWED_MASK) ||
	    segment_count > (data_len - PSLC_HEADER_LEN) /
			    PSLC_SEGMENT_HEADER_LEN)
		return -EINVAL;

	for (i = 0U; i < segment_count; i++) {
		struct ps_pslc_segment_input seg;
		u32 data_part;
		u32 padding;
		u32 j;
		u32 state;

		if (offset > data_len ||
		    data_len - offset < PSLC_SEGMENT_HEADER_LEN)
			return -EINVAL;
		memset(&seg, 0, sizeof(seg));
		seg.kind = ps_get_le16(data + offset);
		seg.flags = ps_get_le16(data + offset + 2U);
		if (ps_get_le16(data + offset + 4U) != PSLC_SEGMENT_HEADER_LEN ||
		    ps_get_le16(data + offset + 6U))
			return -EINVAL;
		seg.ordinal = ps_get_le32(data + offset + 8U);
		seg.pstore_type = ps_get_le32(data + offset + 12U);
		seg.record_id = ps_get_le64(data + offset + 16U);
		seg.timestamp_sec = ps_get_le64(data + offset + 24U);
		seg.timestamp_nsec = ps_get_le32(data + offset + 32U);
		seg.payload_len = ps_get_le32(data + offset + 36U);
		seg.ecc_len = ps_get_le32(data + offset + 40U);
		offset += PSLC_SEGMENT_HEADER_LEN;
		if (ps_add_u32(seg.payload_len, seg.ecc_len, &data_part) ||
		    data_part > data_len - offset)
			return -EINVAL;
		seg.payload = data + offset;
		seg.ecc = data + offset + seg.payload_len;
		if (ps_segment_semantics(&seg))
			return -EINVAL;
		state = ps_crc32_feed(ps_crc32_begin(), seg.payload,
				      seg.payload_len);
		state = ps_crc32_feed(state, seg.ecc, seg.ecc_len);
		if (ps_crc32_end(state) != ps_get_le32(data + offset - 4U))
			return -EINVAL;
		padding = (4U - (data_part & 3U)) & 3U;
		if (padding > data_len - offset - data_part)
			return -EINVAL;
		for (j = 0U; j < padding; j++)
			if (data[offset + data_part + j])
				return -EINVAL;

		if (i == 0U && seg.kind != PSLC_SEGMENT_KIND_PANIC_DESCRIPTION)
			return -EINVAL;
		if (i == 1U && seg.kind != PSLC_SEGMENT_KIND_CURRENT_KMSG)
			return -EINVAL;
		if (i > 1U && seg.kind != PSLC_SEGMENT_KIND_PSTORE_RECORD)
			return -EINVAL;
		if (seg.kind == PSLC_SEGMENT_KIND_CURRENT_KMSG) {
			current_payload_len = seg.payload_len;
			current_flags = seg.flags;
			if (context->current_full) {
				u32 suffix;

				if (context->current_full_len != current_total_len ||
				    current_payload_len > current_total_len)
					return -EINVAL;
				suffix = current_total_len - current_payload_len;
				if (current_payload_len &&
				    memcmp(seg.payload, context->current_full + suffix,
					   current_payload_len))
					return -EINVAL;
			}
		}
		if (seg.kind == PSLC_SEGMENT_KIND_PSTORE_RECORD) {
			struct ps_parsed_pstore_order order = {
				.timestamp_sec = seg.timestamp_sec,
				.record_id = seg.record_id,
				.timestamp_nsec = seg.timestamp_nsec,
				.pstore_type = seg.pstore_type,
				.ordinal = seg.ordinal,
				.valid = 1U,
			};

			if (!ps_parsed_ordered(&previous, &order))
				return -EINVAL;
			previous = order;
			pstore_count++;
		}
		offset += data_part + padding;
	}
	if (offset != data_len)
		return -EINVAL;

	memset(&synthetic, 0, sizeof(synthetic));
	synthetic.container_flags = container_flags;
	synthetic.mirror_source = context->mirror_source;
	synthetic.mirror_events = context->mirror_events;
	synthetic.tail_capacity = context->tail_capacity;
	synthetic.current_total_len = current_total_len;
	synthetic.pstore_total_count = pstore_total_count;
	if (ps_expected_container_flags(&synthetic, current_payload_len,
					pstore_count, &expected_flags) ||
	    expected_flags != container_flags ||
	    ((current_flags & PSLC_SEGMENT_FLAG_TRUNCATED) != 0U) !=
		(current_total_len > current_payload_len))
		return -EINVAL;

	memset(summary, 0, sizeof(*summary));
	summary->container_flags = container_flags;
	summary->current_total_len = current_total_len;
	summary->pstore_total_count = pstore_total_count;
	summary->current_payload_len = current_payload_len;
	summary->serialized_pstore_count = pstore_count;
	summary->segment_count = segment_count;
	summary->total_len = total_len;
	return 0;
}

static void ps_hex8(char *out, u32 value)
{
	static const char hex[] = "0123456789ABCDEF";
	u32 i;

	for (i = 0U; i < 8U; i++)
		out[i] = hex[(value >> ((7U - i) * 4U)) & 0x0fU];
}

static void ps_status_line(char *line, u32 stride, const char *prefix,
			   u32 prefix_len, u32 value)
{
	memset(line, 0, stride);
	memcpy(line, prefix, prefix_len);
	ps_hex8(line + prefix_len, value);
}

int ps_pslc_status_lines(const struct ps_pslc_summary *summary, char *out,
			 u32 stride)
{
	if (!summary || !out || stride < 12U)
		return -EINVAL;
	ps_status_line(out + 0U * stride, stride, "C S", 3U,
		       summary->current_payload_len);
	ps_status_line(out + 1U * stride, stride, "C T", 3U,
		       summary->current_total_len);
	ps_status_line(out + 2U * stride, stride, "P S", 3U,
		       summary->serialized_pstore_count);
	ps_status_line(out + 3U * stride, stride, "P T", 3U,
		       summary->pstore_total_count);
	ps_status_line(out + 4U * stride, stride, "F ", 2U,
		       summary->container_flags);
	return 0;
}
