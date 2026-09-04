// SPDX-License-Identifier: GPL-2.0-only
#include "pstore_screen_log_internal.h"

#ifndef PSTORE_SCREEN_HOST
#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/limits.h>
#include <linux/pstore_screen_log.h>
#include <linux/spinlock.h>

#if IS_ENABLED(CONFIG_PSTORE_SCREEN_PSTORE_MIRROR)
#include <linux/pstore.h>
#endif
#endif

int ps_log_checked_add_u32(u32 left, u32 right, u32 *out)
{
	if (!out || right > 0xffffffffU - left)
		return -EOVERFLOW;
	*out = left + right;
	return 0;
}

static void ps_log_reverse(u8 *data, u32 first, u32 last)
{
	while (first < last) {
		u8 value = data[first];

		data[first] = data[last];
		data[last] = value;
		first++;
		last--;
	}
}

static void ps_log_rotate_left(u8 *data, u32 length, u32 shift)
{
	if (!shift || shift >= length)
		return;
	ps_log_reverse(data, 0U, shift - 1U);
	ps_log_reverse(data, shift, length - 1U);
	ps_log_reverse(data, 0U, length - 1U);
}

static int ps_log_capture_fail(u8 *scratch, u32 scratch_len,
			       struct ps_log_capture_result *result, int error)
{
	if (scratch && scratch_len >= PS_LOG_SCRATCH_BYTES)
		memset(scratch, 0, PS_LOG_SCRATCH_BYTES);
	if (result)
		memset(result, 0, sizeof(*result));
	return error;
}

int ps_log_capture_frozen_window(u64 start_seq, u64 end_seq,
				 const struct ps_log_reader_ops *ops, void *ctx,
				 u8 *scratch, u32 scratch_len,
				 struct ps_log_capture_result *result)
{
	u8 *tail;
	u8 *record;
	u64 expected;
	u32 total = 0U;
	u32 write_offset = 0U;

	if (!result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	if (!ops || !ops->cursor || !ops->get_line || !scratch ||
	    scratch_len < PS_LOG_SCRATCH_BYTES || start_seq > end_seq)
		return ps_log_capture_fail(scratch, scratch_len, result, -EINVAL);

	tail = scratch;
	record = scratch + PS_LOG_TAIL_BYTES;
	memset(tail, 0, PS_LOG_TAIL_BYTES);
	expected = start_seq;
	while (expected < end_seq) {
		u64 post_cursor;
		u32 line_len = 0U;
		u32 next_total;
		u32 first;
		int got;

		if (ops->cursor(ctx) != expected)
			return ps_log_capture_fail(scratch, scratch_len, result,
					   -ESTALE);
		record[PS_MAX_FORMATTED_RECORD_BYTES] = PS_LOG_GUARD_VALUE;
		got = ops->get_line(ctx, 1U, record, PS_LOG_GET_LINE_BYTES,
				    &line_len);
		post_cursor = ops->cursor(ctx);
		if (got <= 0)
			return ps_log_capture_fail(scratch, scratch_len, result,
					   got < 0 ? got : -ENODATA);
		if (post_cursor != expected + 1U || post_cursor > end_seq ||
		    post_cursor - 1U != expected)
			return ps_log_capture_fail(scratch, scratch_len, result,
					   -ESTALE);
		if (!line_len || line_len > PS_MAX_FORMATTED_RECORD_BYTES)
			return ps_log_capture_fail(scratch, scratch_len, result,
					   -EMSGSIZE);
		if (record[line_len] != 0U ||
		    (line_len < PS_MAX_FORMATTED_RECORD_BYTES &&
		     record[PS_MAX_FORMATTED_RECORD_BYTES] !=
			PS_LOG_GUARD_VALUE))
			return ps_log_capture_fail(scratch, scratch_len, result,
					   -EILSEQ);
		if (ps_log_checked_add_u32(total, line_len, &next_total))
			return ps_log_capture_fail(scratch, scratch_len, result,
					   -EOVERFLOW);

		first = PS_LOG_TAIL_BYTES - write_offset;
		if (first > line_len)
			first = line_len;
		memcpy(tail + write_offset, record, first);
		if (line_len > first)
			memcpy(tail, record + first, line_len - first);
		write_offset = (write_offset + line_len) % PS_LOG_TAIL_BYTES;
		total = next_total;
		expected = post_cursor;
		if (ops->checkpoint)
			ops->checkpoint(ctx);
	}
	if (expected != end_seq || ops->cursor(ctx) != end_seq)
		return ps_log_capture_fail(scratch, scratch_len, result, -ESTALE);

	result->payload_len = total < PS_LOG_TAIL_BYTES ?
			      total : PS_LOG_TAIL_BYTES;
	if (total >= PS_LOG_TAIL_BYTES && write_offset)
		ps_log_rotate_left(tail, PS_LOG_TAIL_BYTES, write_offset);
	result->payload = tail;
	result->current_total_len = total;
	result->start_seq = start_seq;
	result->end_seq = end_seq;
	result->valid = 1U;
	return 0;
}

static int ps_log_pstore_type_valid(u32 type)
{
	return (type >= PSLC_PSTORE_TYPE_DMESG &&
		type <= PSLC_PSTORE_TYPE_PPC_OPAL) ||
	       type == PSLC_PSTORE_TYPE_UNKNOWN;
}

static void ps_log_mirror_event(struct ps_log_mirror_image *image, u32 event)
{
	if (image)
		image->events |= event;
}

void ps_log_mirror_reset(struct ps_log_mirror_image *image, u32 source)
{
	struct ps_log_mirror_record *records;
	u32 record_capacity;
	u8 *arena;
	u32 arena_capacity;

	if (!image)
		return;
	records = image->records;
	record_capacity = image->record_capacity;
	arena = image->arena;
	arena_capacity = image->arena_capacity;
	memset(image, 0, sizeof(*image));
	image->records = records;
	image->record_capacity = record_capacity;
	image->arena = arena;
	image->arena_capacity = arena_capacity;
	image->source = source;
	image->valid = source <= PS_MIRROR_SOURCE_CERTIFIED_INCOMPLETE ? 1U : 0U;
	if (records && record_capacity)
		memset(records, 0, sizeof(*records) * record_capacity);
	if (arena && arena_capacity)
		memset(arena, 0, arena_capacity);
}

static int ps_log_mirror_input_valid(const struct ps_log_pstore_input *input)
{
	if (!input || !ps_log_pstore_type_valid(input->pstore_type) ||
	    input->timestamp_nsec >= 1000000000U ||
	    input->original_compressed > 1U || input->remains_compressed > 1U ||
	    (!input->payload && input->payload_len) ||
	    (!input->ecc && input->ecc_len) || !input->payload_len)
		return 0;
	if (input->remains_compressed && !input->original_compressed)
		return 0;
	if (input->original_compressed) {
		if (input->codec < PSLC_CODEC_DEFLATE ||
		    input->codec > PSLC_CODEC_ZSTD)
			return 0;
	} else if (input->codec != PSLC_CODEC_NONE) {
		return 0;
	}
	return 1;
}

int ps_log_mirror_append(struct ps_log_mirror_image *image,
			 const struct ps_log_pstore_input *input)
{
	struct ps_log_mirror_record *record;
	u32 ordinal;
	u32 next_total;
	u32 bytes;

	if (!image || !image->valid ||
	    image->source == PS_MIRROR_SOURCE_ABSENT)
		return -EINVAL;
	ordinal = image->total_count;
	if (ps_log_checked_add_u32(image->total_count, 1U, &next_total)) {
		image->valid = 0U;
		return -EOVERFLOW;
	}
	image->total_count = next_total;
	if (!ps_log_mirror_input_valid(input)) {
		ps_log_mirror_event(image, PS_MIRROR_EVENT_COPY_FAILURE);
		return -EINVAL;
	}
	if (ps_log_checked_add_u32(input->payload_len, input->ecc_len, &bytes)) {
		ps_log_mirror_event(image, PS_MIRROR_EVENT_COPY_FAILURE);
		return -EOVERFLOW;
	}
	if (!image->records || !image->arena ||
	    image->record_count >= image->record_capacity ||
	    bytes > image->arena_capacity - image->arena_len) {
		ps_log_mirror_event(image, PS_MIRROR_EVENT_OVERFLOW);
		return -ENOSPC;
	}

	record = &image->records[image->record_count];
	memset(record, 0, sizeof(*record));
	record->ordinal = ordinal;
	record->pstore_type = input->pstore_type;
	record->record_id = input->record_id;
	record->timestamp_sec = input->timestamp_sec;
	record->timestamp_nsec = input->timestamp_nsec;
	record->payload_offset = image->arena_len;
	record->payload_len = input->payload_len;
	record->ecc_offset = image->arena_len + input->payload_len;
	record->ecc_len = input->ecc_len;
	record->count = input->count;
	record->reason = input->reason;
	record->part = input->part;
	if (input->original_compressed) {
		record->flags |= PSLC_SEGMENT_FLAG_COMPRESSED |
			(input->codec << PSLC_SEGMENT_CODEC_SHIFT);
		if (input->remains_compressed)
			record->flags |= PSLC_SEGMENT_FLAG_DECOMPRESS_ERROR |
				PSLC_SEGMENT_FLAG_PAYLOAD_IS_RAW_COMPRESSED;
	}
	if (input->ecc_len)
		record->flags |= PSLC_SEGMENT_FLAG_HAS_ECC;
	memcpy(image->arena + image->arena_len, input->payload,
	       input->payload_len);
	if (input->ecc_len)
		memcpy(image->arena + record->ecc_offset, input->ecc,
		       input->ecc_len);
	image->arena_len += bytes;
	image->record_count++;
	return 0;
}

static int ps_log_record_before(const struct ps_log_mirror_record *left,
				const struct ps_log_mirror_record *right)
{
	if (left->timestamp_sec != right->timestamp_sec)
		return left->timestamp_sec < right->timestamp_sec;
	if (left->timestamp_nsec != right->timestamp_nsec)
		return left->timestamp_nsec < right->timestamp_nsec;
	if (left->pstore_type != right->pstore_type)
		return left->pstore_type < right->pstore_type;
	if (left->record_id != right->record_id)
		return left->record_id < right->record_id;
	return left->ordinal < right->ordinal;
}

static void ps_log_mirror_sort(struct ps_log_mirror_image *image)
{
	u32 i;

	for (i = 1U; i < image->record_count; i++) {
		struct ps_log_mirror_record value = image->records[i];
		u32 at = i;

		while (at && ps_log_record_before(&value,
						  &image->records[at - 1U])) {
			image->records[at] = image->records[at - 1U];
			at--;
		}
		image->records[at] = value;
	}
}

int ps_log_mirror_validate(const struct ps_log_mirror_image *image)
{
	u32 i;

	if (!image || !image->valid ||
	    (image->events & ~PS_MIRROR_EVENTS_ALLOWED_MASK) ||
	    image->record_count > image->record_capacity ||
	    image->arena_len > image->arena_capacity ||
	    image->total_count < image->record_count)
		return -EINVAL;
	if (image->source == PS_MIRROR_SOURCE_ABSENT) {
		if (image->events || image->total_count || image->record_count ||
		    image->arena_len)
			return -EINVAL;
		return 0;
	}
	if (image->source == PS_MIRROR_SOURCE_CERTIFIED_COMPLETE) {
		if (image->events)
			return -EINVAL;
	} else if (image->source == PS_MIRROR_SOURCE_CERTIFIED_INCOMPLETE) {
		if (!image->events)
			return -EINVAL;
	} else {
		return -EINVAL;
	}
	for (i = 0U; i < image->record_count; i++) {
		const struct ps_log_mirror_record *record = &image->records[i];
		u32 end;

		if (!ps_log_pstore_type_valid(record->pstore_type) ||
		    record->timestamp_nsec >= 1000000000U ||
		    ps_log_checked_add_u32(record->payload_offset,
					   record->payload_len, &end) ||
		    end != record->ecc_offset ||
		    ps_log_checked_add_u32(end, record->ecc_len, &end) ||
		    end > image->arena_len)
			return -EINVAL;
		if (i && ps_log_record_before(record, &image->records[i - 1U]))
			return -EINVAL;
	}
	return 0;
}

int ps_log_mirror_finish(struct ps_log_mirror_image *image, u32 events)
{
	if (!image || !image->valid ||
	    (events & ~PS_MIRROR_EVENTS_ALLOWED_MASK))
		return -EINVAL;
	if (image->source == PS_MIRROR_SOURCE_ABSENT)
		return ps_log_mirror_validate(image);
	image->events |= events;
	image->source = image->events ?
		PS_MIRROR_SOURCE_CERTIFIED_INCOMPLETE :
		PS_MIRROR_SOURCE_CERTIFIED_COMPLETE;
	ps_log_mirror_sort(image);
	return ps_log_mirror_validate(image);
}

static u32 ps_log_reason_flags(u32 events)
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

static u32 ps_log_container_flags(const struct ps_log_capture_result *capture,
				  const struct ps_log_mirror_image *mirror,
				  u32 serialized_count)
{
	u32 flags = 0U;

	if (capture->current_total_len > capture->payload_len)
		flags |= PSLC_CONTAINER_FLAG_TRUNCATED;
	if (!capture->current_total_len)
		flags |= PSLC_CONTAINER_FLAG_CURRENT_KMSG_EMPTY;
	if (mirror->source == PS_MIRROR_SOURCE_ABSENT) {
		flags |= PSLC_CONTAINER_FLAG_RAMOOPS_UNAVAILABLE;
	} else {
		if (mirror->total_count > serialized_count)
			flags |= PSLC_CONTAINER_FLAG_PSTORE_DROPPED |
				 PSLC_CONTAINER_FLAG_TRUNCATED;
		if (mirror->events)
			flags |= PSLC_CONTAINER_FLAG_MIRROR_INCOMPLETE |
				 ps_log_reason_flags(mirror->events) |
				 PSLC_CONTAINER_FLAG_TRUNCATED;
	}
	return flags;
}

int ps_log_build_pslc(const u8 *description, u32 description_len,
		      const struct ps_log_capture_result *capture,
		      const struct ps_log_mirror_image *mirror,
		      struct ps_pslc_segment_input *segments,
		      u32 segment_capacity, u8 *out, u32 out_capacity,
		      u32 *out_len, struct ps_pslc_summary *summary)
{
	struct ps_pslc_build_input input;
	u32 flags;
	u32 expected_payload;
	u32 serialized_count;
	u32 measured;
	u32 i;
	int ret;
	bool total_empty;
	bool payload_empty;

	if ((!description && description_len) ||
	    description_len >= PS_LOG_PANIC_DESCRIPTION_BYTES || !capture ||
	    !capture->valid || !mirror || !segments || !out || !out_len ||
	    segment_capacity < 2U + mirror->record_count ||
	    ps_log_mirror_validate(mirror))
		return -EINVAL;
	expected_payload = capture->current_total_len < PS_LOG_TAIL_BYTES ?
			   capture->current_total_len : PS_LOG_TAIL_BYTES;
	total_empty = capture->current_total_len == 0U;
	payload_empty = capture->payload_len == 0U;
	if (capture->payload_len != expected_payload ||
	    total_empty != payload_empty ||
	    (!capture->payload && capture->payload_len))
		return -EINVAL;

	memset(segments, 0,
	       sizeof(*segments) * (2U + mirror->record_count));
	segments[0].kind = PSLC_SEGMENT_KIND_PANIC_DESCRIPTION;
	segments[0].payload = description;
	segments[0].payload_len = description_len;
	segments[1].kind = PSLC_SEGMENT_KIND_CURRENT_KMSG;
	segments[1].payload = capture->payload;
	segments[1].payload_len = capture->payload_len;
	if (capture->current_total_len > capture->payload_len)
		segments[1].flags = PSLC_SEGMENT_FLAG_TRUNCATED;

	for (i = 0U; i < mirror->record_count; i++) {
		const struct ps_log_mirror_record *record = &mirror->records[i];
		struct ps_pslc_segment_input *segment = &segments[2U + i];

		segment->kind = PSLC_SEGMENT_KIND_PSTORE_RECORD;
		segment->flags = record->flags;
		segment->ordinal = record->ordinal;
		segment->pstore_type = record->pstore_type;
		segment->record_id = record->record_id;
		segment->timestamp_sec = record->timestamp_sec;
		segment->timestamp_nsec = record->timestamp_nsec;
		segment->payload_len = record->payload_len;
		segment->ecc_len = record->ecc_len;
		segment->payload = mirror->arena + record->payload_offset;
		segment->ecc = record->ecc_len ?
			mirror->arena + record->ecc_offset : NULL;
	}
	memset(&input, 0, sizeof(input));
	input.mirror_source = mirror->source;
	input.mirror_events = mirror->events;
	input.tail_capacity = PS_LOG_TAIL_BYTES;
	input.current_total_len = capture->current_total_len;
	input.pstore_total_count = mirror->total_count;
	input.segments = segments;
	serialized_count = mirror->record_count;
	for (;;) {
		flags = ps_log_container_flags(capture, mirror, serialized_count);
		input.container_flags = flags;
		input.segment_count = 2U + serialized_count;
		ret = ps_pslc_measure(&input, &measured);
		if (ret)
			return ret;
		if (measured <= out_capacity)
			break;
		if (!serialized_count)
			return -ENOSPC;
		serialized_count--;
	}
	ret = ps_pslc_build(&input, out, out_capacity, out_len);
	if (ret)
		return ret;
	if (summary) {
		memset(summary, 0, sizeof(*summary));
		summary->container_flags = flags;
		summary->current_total_len = capture->current_total_len;
		summary->pstore_total_count = mirror->total_count;
		summary->current_payload_len = capture->payload_len;
		summary->serialized_pstore_count = serialized_count;
		summary->segment_count = input.segment_count;
		summary->total_len = *out_len;
	}
	return 0;
}

#ifndef PSTORE_SCREEN_HOST

static u8 ps_log_kmsg_scratch[PS_LOG_SCRATCH_BYTES];
static struct ps_pslc_segment_input ps_log_segments[PS_LOG_MAX_SEGMENTS];
static u8 ps_log_container[PS_LOG_CONTAINER_BYTES];
static struct ps_pslc_summary ps_log_container_summary;
static u32 ps_log_container_len;
static bool ps_log_container_valid;

static u8 ps_log_panic_description[PS_LOG_PANIC_DESCRIPTION_BYTES];
static u32 ps_log_panic_description_len;
static bool ps_log_panic_description_invalid;
static u32 ps_log_panic_description_state;
static u32 ps_log_dump_state;

static struct ps_log_mirror_image ps_log_frozen_mirror;

#if IS_ENABLED(CONFIG_PSTORE_SCREEN_PSTORE_MIRROR)

struct ps_log_kernel_mirror_slot {
	u32 sequence;
	struct ps_log_mirror_image image;
	struct ps_log_mirror_record records[PS_LOG_MAX_PSTORE_RECORDS];
	u8 arena[PS_LOG_PSTORE_BYTES];
};

static struct ps_log_kernel_mirror_slot
	ps_log_mirror_slots[PS_LOG_MIRROR_SLOT_COUNT];
static struct ps_log_mirror_record
	ps_log_frozen_records[PS_LOG_MAX_PSTORE_RECORDS];
static u8 ps_log_frozen_arena[PS_LOG_PSTORE_BYTES];
static DEFINE_SPINLOCK(ps_log_mirror_writer_lock);
static u32 active_slot = PS_LOG_SLOT_NONE;
static u32 previous_slot = PS_LOG_SLOT_NONE;
static u32 inactive_slot = PS_LOG_SLOT_NONE;
static u32 ps_log_mirror_writer_slot = PS_LOG_SLOT_NONE;
static bool ps_log_mirror_writer_active;
static bool ps_log_mirror_frozen;
static atomic_t ps_log_mirror_gap_count = ATOMIC_INIT(0);
static bool ps_log_mirror_invalidating;
static u32 ps_log_mirror_invalidate_epoch;
static u32 ps_log_mirror_invalidate_active = PS_LOG_SLOT_NONE;
static u32 ps_log_mirror_invalidate_previous = PS_LOG_SLOT_NONE;
#if defined(CONFIG_PSTORE_SCREEN_LOG_KUNIT_TEST)
static bool ps_log_mirror_invalidate_interleave_for_test;
static bool ps_log_mirror_freeze_interleave_for_test;
#endif

static void ps_log_mirror_note_gap(void)
{
	atomic_add_unless(&ps_log_mirror_gap_count, 1, INT_MAX);
}

static void ps_log_mirror_scan_lock_missed(void)
{
	if (!READ_ONCE(ps_log_mirror_frozen) &&
	    /* Pair with invalidate's release publication before noting a gap. */
	    !smp_load_acquire(&ps_log_mirror_invalidating))
		ps_log_mirror_note_gap();
}

static void ps_log_prepare_mirror_image(struct ps_log_mirror_image *image,
					struct ps_log_mirror_record *records,
					u8 *arena)
{
	memset(image, 0, sizeof(*image));
	image->records = records;
	image->record_capacity = PS_LOG_MAX_PSTORE_RECORDS;
	image->arena = arena;
	image->arena_capacity = PS_LOG_PSTORE_BYTES;
	ps_log_mirror_reset(image, PS_MIRROR_SOURCE_ABSENT);
}

static void ps_log_mirror_state_init(void)
{
	u32 i;

	for (i = 0U; i < PS_LOG_MIRROR_SLOT_COUNT; i++) {
		WRITE_ONCE(ps_log_mirror_slots[i].sequence, 0U);
		ps_log_prepare_mirror_image(&ps_log_mirror_slots[i].image,
					    ps_log_mirror_slots[i].records,
					    ps_log_mirror_slots[i].arena);
	}
	ps_log_prepare_mirror_image(&ps_log_frozen_mirror,
				    ps_log_frozen_records,
				    ps_log_frozen_arena);
	WRITE_ONCE(active_slot, PS_LOG_SLOT_NONE);
	WRITE_ONCE(previous_slot, PS_LOG_SLOT_NONE);
	WRITE_ONCE(inactive_slot, PS_LOG_SLOT_NONE);
	WRITE_ONCE(ps_log_mirror_writer_slot, PS_LOG_SLOT_NONE);
	WRITE_ONCE(ps_log_mirror_writer_active, false);
	WRITE_ONCE(ps_log_mirror_frozen, false);
	atomic_set(&ps_log_mirror_gap_count, 0);
	WRITE_ONCE(ps_log_mirror_invalidating, false);
	WRITE_ONCE(ps_log_mirror_invalidate_epoch, 0U);
	WRITE_ONCE(ps_log_mirror_invalidate_active, PS_LOG_SLOT_NONE);
	WRITE_ONCE(ps_log_mirror_invalidate_previous, PS_LOG_SLOT_NONE);
#if defined(CONFIG_PSTORE_SCREEN_LOG_KUNIT_TEST)
	WRITE_ONCE(ps_log_mirror_invalidate_interleave_for_test, false);
	WRITE_ONCE(ps_log_mirror_freeze_interleave_for_test, false);
#endif
}

static u32 ps_log_choose_inactive_slot(void)
{
	u32 active = READ_ONCE(active_slot);
	u32 previous = READ_ONCE(previous_slot);
	u32 i;

	for (i = 0U; i < PS_LOG_MIRROR_SLOT_COUNT; i++) {
		if (i != active && i != previous)
			return i;
	}
	return PS_LOG_SLOT_NONE;
}

static void ps_log_abort_writer_locked(void)
{
	u32 slot = READ_ONCE(ps_log_mirror_writer_slot);
	u32 sequence;

	if (slot < PS_LOG_MIRROR_SLOT_COUNT) {
		sequence = READ_ONCE(ps_log_mirror_slots[slot].sequence);
		if (sequence & 1U)
			WRITE_ONCE(ps_log_mirror_slots[slot].sequence,
				   sequence + 1U);
	}
	WRITE_ONCE(ps_log_mirror_writer_slot, PS_LOG_SLOT_NONE);
	/* Publish the abort after restoring an even sequence and no slot. */
	smp_store_release(&ps_log_mirror_writer_active, false);
}

void pstore_screen_log_mirror_scan_begin(void)
{
	struct ps_log_kernel_mirror_slot *slot;
	unsigned long flags;
	u32 sequence;
	u32 selected;

	if (READ_ONCE(ps_log_mirror_frozen) ||
	    /* Observe invalidation before trying to enter its writer epoch. */
	    smp_load_acquire(&ps_log_mirror_invalidating))
		return;
	if (!spin_trylock_irqsave(&ps_log_mirror_writer_lock, flags)) {
		ps_log_mirror_scan_lock_missed();
		return;
	}
	if (READ_ONCE(ps_log_mirror_frozen) ||
	    /* Recheck release-published invalidation under the writer lock. */
	    smp_load_acquire(&ps_log_mirror_invalidating) ||
	    READ_ONCE(ps_log_mirror_writer_active)) {
		if (!READ_ONCE(ps_log_mirror_frozen) &&
		    !READ_ONCE(ps_log_mirror_invalidating))
			ps_log_mirror_note_gap();
		spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);
		return;
	}

	selected = ps_log_choose_inactive_slot();
	if (selected >= PS_LOG_MIRROR_SLOT_COUNT) {
		ps_log_mirror_note_gap();
		spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);
		return;
	}
	inactive_slot = selected;
	slot = &ps_log_mirror_slots[selected];
	sequence = READ_ONCE(slot->sequence);
	if (sequence & 1U)
		sequence++;
	WRITE_ONCE(slot->sequence, sequence + 1U);
	WRITE_ONCE(ps_log_mirror_writer_slot, selected);
	/* Publish the odd sequence and selected slot before record callbacks. */
	smp_store_release(&ps_log_mirror_writer_active, true);
	spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);

	ps_log_mirror_reset(&slot->image,
			    PS_MIRROR_SOURCE_CERTIFIED_COMPLETE);
	if (READ_ONCE(ps_log_mirror_frozen)) {
		spin_lock_irqsave(&ps_log_mirror_writer_lock, flags);
		if (READ_ONCE(ps_log_mirror_writer_slot) == selected)
			ps_log_abort_writer_locked();
		spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);
	}
}

static u32 ps_log_private_pstore_type(enum pstore_type_id type)
{
	switch (type) {
	case PSTORE_TYPE_DMESG:
		return PSLC_PSTORE_TYPE_DMESG;
	case PSTORE_TYPE_CONSOLE:
		return PSLC_PSTORE_TYPE_CONSOLE;
	case PSTORE_TYPE_FTRACE:
		return PSLC_PSTORE_TYPE_FTRACE;
	case PSTORE_TYPE_PMSG:
		return PSLC_PSTORE_TYPE_PMSG;
	case PSTORE_TYPE_MCE:
		return PSLC_PSTORE_TYPE_MCE;
	case PSTORE_TYPE_PPC_RTAS:
		return PSLC_PSTORE_TYPE_PPC_RTAS;
	case PSTORE_TYPE_PPC_OF:
		return PSLC_PSTORE_TYPE_PPC_OF;
	case PSTORE_TYPE_PPC_COMMON:
		return PSLC_PSTORE_TYPE_PPC_COMMON;
	case PSTORE_TYPE_PPC_OPAL:
		return PSLC_PSTORE_TYPE_PPC_OPAL;
	default:
		return PSLC_PSTORE_TYPE_UNKNOWN;
	}
}

static u32 ps_log_private_codec(const char *name)
{
	if (!name)
		return PSLC_CODEC_NONE;
	if (!strcmp(name, "deflate"))
		return PSLC_CODEC_DEFLATE;
	if (!strcmp(name, "lzo"))
		return PSLC_CODEC_LZO;
	if (!strcmp(name, "lz4") || !strcmp(name, "lz4hc"))
		return PSLC_CODEC_LZ4;
	if (!strcmp(name, "zstd"))
		return PSLC_CODEC_ZSTD;
	return PSLC_CODEC_NONE;
}

void pstore_screen_log_mirror_record(const struct pstore_record *record,
				     bool was_compressed,
				     const char *codec_name)
{
	struct ps_log_pstore_input input;
	struct ps_log_kernel_mirror_slot *slot;
	u32 selected;

	if (READ_ONCE(ps_log_mirror_frozen))
		return;
	/* Pair with scan_begin's publication of the selected writer slot. */
	if (!smp_load_acquire(&ps_log_mirror_writer_active))
		return;
	selected = READ_ONCE(ps_log_mirror_writer_slot);
	if (selected >= PS_LOG_MIRROR_SLOT_COUNT)
		return;
	slot = &ps_log_mirror_slots[selected];
	memset(&input, 0, sizeof(input));
	if (!record || !record->buf || record->size <= 0 ||
	    record->size > 0xffffffffLL || record->ecc_notice_size < 0 ||
	    record->ecc_notice_size > 0xffffffffLL) {
		ps_log_mirror_append(&slot->image, &input);
		return;
	}
	input.pstore_type = ps_log_private_pstore_type(record->type);
	input.original_compressed = was_compressed ? 1U : 0U;
	input.remains_compressed = record->compressed ? 1U : 0U;
	input.codec = was_compressed ? ps_log_private_codec(codec_name) :
		PSLC_CODEC_NONE;
	input.record_id = record->id;
	input.timestamp_sec = (u64)record->time.tv_sec;
	input.timestamp_nsec = (u32)record->time.tv_nsec;
	input.count = (u32)record->count;
	input.reason = (u32)record->reason;
	input.part = record->part;
	input.payload = (const u8 *)record->buf;
	input.payload_len = (u32)record->size;
	input.ecc = (const u8 *)record->buf + record->size;
	input.ecc_len = (u32)record->ecc_notice_size;
	ps_log_mirror_append(&slot->image, &input);
}

void pstore_screen_log_mirror_scan_end(u32 events)
{
	struct ps_log_kernel_mirror_slot *slot;
	unsigned long flags;
	int gap_snapshot;
	int finish_ret;
	u32 old_active;
	u32 selected;
	u32 sequence;

	/* Pair with scan_begin before consuming the normal-context image. */
	if (!smp_load_acquire(&ps_log_mirror_writer_active))
		return;
	selected = READ_ONCE(ps_log_mirror_writer_slot);
	if (selected >= PS_LOG_MIRROR_SLOT_COUNT)
		return;
	slot = &ps_log_mirror_slots[selected];
	gap_snapshot = atomic_read(&ps_log_mirror_gap_count);
	if (gap_snapshot > 0)
		events |= PS_MIRROR_EVENT_GENERATION_GAP;
	finish_ret = ps_log_mirror_finish(&slot->image, events);

	spin_lock_irqsave(&ps_log_mirror_writer_lock, flags);
	if (!READ_ONCE(ps_log_mirror_writer_active) ||
	    READ_ONCE(ps_log_mirror_writer_slot) != selected) {
		spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);
		return;
	}
	if (finish_ret || READ_ONCE(ps_log_mirror_frozen)) {
		if (finish_ret)
			ps_log_mirror_note_gap();
		ps_log_abort_writer_locked();
		spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);
		return;
	}
	sequence = READ_ONCE(slot->sequence);
	if (!(sequence & 1U)) {
		ps_log_mirror_note_gap();
		ps_log_abort_writer_locked();
		spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);
		return;
	}
	/* Publish the complete image before exposing its even sequence. */
	smp_store_release(&slot->sequence, sequence + 1U);
	/* Read the last published generation before rotating the slots. */
	old_active = smp_load_acquire(&active_slot);
	WRITE_ONCE(previous_slot, old_active);
	/* Publish the new active slot after its image and sequence are stable. */
	smp_store_release(&active_slot, selected);
	WRITE_ONCE(inactive_slot, ps_log_choose_inactive_slot());
	WRITE_ONCE(ps_log_mirror_writer_slot, PS_LOG_SLOT_NONE);
	/* Retire writer state only after the active generation is published. */
	smp_store_release(&ps_log_mirror_writer_active, false);
	if (gap_snapshot > 0)
		atomic_cmpxchg(&ps_log_mirror_gap_count, gap_snapshot, 0);
	spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);
}

void pstore_screen_log_mirror_invalidate(void)
{
	unsigned long flags;
	u32 epoch;
	u32 saved_active;
	u32 saved_previous;

	if (READ_ONCE(ps_log_mirror_frozen))
		return;

	spin_lock_irqsave(&ps_log_mirror_writer_lock, flags);
	if (READ_ONCE(ps_log_mirror_frozen)) {
		spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);
		return;
	}
	saved_active = READ_ONCE(active_slot);
	saved_previous = READ_ONCE(previous_slot);
	epoch = READ_ONCE(ps_log_mirror_invalidate_epoch);
	WRITE_ONCE(ps_log_mirror_invalidate_active, saved_active);
	WRITE_ONCE(ps_log_mirror_invalidate_previous, saved_previous);
	/* Publish fallback snapshots before hiding the active generation. */
	smp_store_release(&ps_log_mirror_invalidating, true);
	/* Odd epochs cover the interval where slot metadata is transient. */
	smp_store_release(&ps_log_mirror_invalidate_epoch, epoch + 1U);
	ps_log_abort_writer_locked();
	WRITE_ONCE(previous_slot, PS_LOG_SLOT_NONE);
	/* Hide the active generation after its fallback snapshot is visible. */
	smp_store_release(&active_slot, PS_LOG_SLOT_NONE);
	WRITE_ONCE(inactive_slot, PS_LOG_SLOT_NONE);
	/* Order slot invalidation before checking whether panic froze it. */
	smp_mb();
	if (READ_ONCE(ps_log_mirror_frozen)) {
		WRITE_ONCE(previous_slot, saved_previous);
		/* Restore the frozen generation before ending invalidation. */
		smp_store_release(&active_slot, saved_active);
		WRITE_ONCE(inactive_slot, ps_log_choose_inactive_slot());
	} else {
		atomic_set(&ps_log_mirror_gap_count, 0);
	}
#if defined(CONFIG_PSTORE_SCREEN_LOG_KUNIT_TEST)
	if (READ_ONCE(ps_log_mirror_invalidate_interleave_for_test)) {
		WRITE_ONCE(ps_log_mirror_invalidate_interleave_for_test, false);
		ps_log_mirror_scan_lock_missed();
	}
#endif
	/* Publish stable slot metadata before closing the invalidation epoch. */
	smp_store_release(&ps_log_mirror_invalidate_epoch, epoch + 2U);
	/* Publish either the completed invalidation or frozen restoration. */
	smp_store_release(&ps_log_mirror_invalidating, false);
	spin_unlock_irqrestore(&ps_log_mirror_writer_lock, flags);
}

static int ps_log_copy_mirror_image(const struct ps_log_mirror_image *source,
				    struct ps_log_mirror_image *destination)
{
	struct ps_log_mirror_record *records = destination->records;
	u32 record_capacity = destination->record_capacity;
	u8 *arena = destination->arena;
	u32 arena_capacity = destination->arena_capacity;

	if (ps_log_mirror_validate(source) ||
	    source->record_count > record_capacity ||
	    source->arena_len > arena_capacity)
		return -EINVAL;
	if (source->record_count)
		memcpy(records, source->records,
		       sizeof(*records) * source->record_count);
	if (source->arena_len)
		memcpy(arena, source->arena, source->arena_len);
	memset(destination, 0, sizeof(*destination));
	destination->source = source->source;
	destination->events = source->events;
	destination->valid = source->valid;
	destination->total_count = source->total_count;
	destination->record_count = source->record_count;
	destination->arena_len = source->arena_len;
	destination->records = records;
	destination->record_capacity = record_capacity;
	destination->arena = arena;
	destination->arena_capacity = arena_capacity;
	return ps_log_mirror_validate(destination);
}

static int ps_log_copy_published_slot(u32 selected,
				      struct ps_log_mirror_image *destination)
{
	struct ps_log_kernel_mirror_slot *slot;
	u32 after;
	u32 before;
	int ret;

	if (selected >= PS_LOG_MIRROR_SLOT_COUNT)
		return -ENOENT;
	slot = &ps_log_mirror_slots[selected];
	/* Pair with scan_end's release of the complete even sequence. */
	before = smp_load_acquire(&slot->sequence);
	if (before & 1U)
		return -ESTALE;
	ret = ps_log_copy_mirror_image(&slot->image, destination);
	/* Finish all image reads before validating the sequence again. */
	smp_rmb();
	after = READ_ONCE(slot->sequence);
	if (ret || before != after || (after & 1U))
		return ret ? ret : -ESTALE;
	return 0;
}

static int ps_log_mirror_apply_pending_gap(struct ps_log_mirror_image *destination,
					   bool pending_gap)
{
	if (!pending_gap)
		return 0;
	if (destination->source == PS_MIRROR_SOURCE_ABSENT) {
		ps_log_mirror_reset(destination,
				    PS_MIRROR_SOURCE_CERTIFIED_INCOMPLETE);
		return ps_log_mirror_finish(destination,
					    PS_MIRROR_EVENT_GENERATION_GAP);
	}
	destination->events |= PS_MIRROR_EVENT_GENERATION_GAP;
	destination->source = PS_MIRROR_SOURCE_CERTIFIED_INCOMPLETE;
	return ps_log_mirror_validate(destination);
}

int pstore_screen_log_freeze_mirror_internal(struct ps_log_mirror_image *destination)
{
	bool invalidating;
	bool pending_gap;
	u32 epoch_after;
	u32 epoch_before;
	u32 selected;
	u32 fallback;

	if (!destination)
		return -EINVAL;
	WRITE_ONCE(ps_log_mirror_frozen, true);
	/* Publish freeze before sampling invalidation and slot state. */
	smp_mb();
	pending_gap = atomic_read(&ps_log_mirror_gap_count) > 0;
	/* Acquire fallback snapshots when freeze intersects invalidation. */
	epoch_before = smp_load_acquire(&ps_log_mirror_invalidate_epoch);
	/* Pair with invalidate's release publication of its transient state. */
	invalidating = smp_load_acquire(&ps_log_mirror_invalidating);
#if defined(CONFIG_PSTORE_SCREEN_LOG_KUNIT_TEST)
	if (!invalidating &&
	    READ_ONCE(ps_log_mirror_freeze_interleave_for_test)) {
		WRITE_ONCE(ps_log_mirror_freeze_interleave_for_test, false);
		WRITE_ONCE(ps_log_mirror_invalidate_active,
			   READ_ONCE(active_slot));
		WRITE_ONCE(ps_log_mirror_invalidate_previous,
			   READ_ONCE(previous_slot));
		/* Publish fallback metadata before opening the invalidation window. */
		smp_store_release(&ps_log_mirror_invalidating, true);
		/* The odd epoch makes the slot transition visible to freeze. */
		smp_store_release(&ps_log_mirror_invalidate_epoch,
				  epoch_before + 1U);
		WRITE_ONCE(previous_slot, PS_LOG_SLOT_NONE);
		/* Hide the active generation only after the odd epoch is published. */
		smp_store_release(&active_slot, PS_LOG_SLOT_NONE);
	}
#endif
	if (invalidating || (epoch_before & 1U)) {
		selected = READ_ONCE(ps_log_mirror_invalidate_active);
		fallback = READ_ONCE(ps_log_mirror_invalidate_previous);
	} else {
		/* Pair with scan_end's active publication before copying. */
		selected = smp_load_acquire(&active_slot);
		/* Order fallback metadata after the active generation sample. */
		fallback = smp_load_acquire(&previous_slot);
		/* Detect an invalidate that hid or restored slots while sampled. */
		epoch_after = smp_load_acquire(&ps_log_mirror_invalidate_epoch);
		if (epoch_before != epoch_after || (epoch_after & 1U)) {
			selected = READ_ONCE(ps_log_mirror_invalidate_active);
			fallback = READ_ONCE(ps_log_mirror_invalidate_previous);
		}
	}
	if (selected == PS_LOG_SLOT_NONE) {
		ps_log_mirror_reset(destination, PS_MIRROR_SOURCE_ABSENT);
		return ps_log_mirror_apply_pending_gap(destination, pending_gap);
	}
	if (!ps_log_copy_published_slot(selected, destination))
		return ps_log_mirror_apply_pending_gap(destination, pending_gap);
	if (fallback < PS_LOG_MIRROR_SLOT_COUNT && fallback != selected &&
	    !ps_log_copy_published_slot(fallback, destination))
		return ps_log_mirror_apply_pending_gap(destination, pending_gap);
	ps_log_mirror_reset(destination,
			    PS_MIRROR_SOURCE_CERTIFIED_INCOMPLETE);
	return ps_log_mirror_finish(destination,
				    PS_MIRROR_EVENT_GENERATION_GAP);
}

#if defined(CONFIG_PSTORE_SCREEN_LOG_KUNIT_TEST)
void pstore_screen_log_reset_mirror_for_test(void)
{
	ps_log_mirror_state_init();
}

void pstore_screen_log_mirror_indices_for_test(u32 *active, u32 *previous,
					       u32 *inactive)
{
	if (active)
		*active = READ_ONCE(active_slot);
	if (previous)
		*previous = READ_ONCE(previous_slot);
	if (inactive)
		*inactive = READ_ONCE(inactive_slot);
}

void pstore_screen_log_corrupt_active_sequence_for_test(void)
{
	u32 selected = READ_ONCE(active_slot);
	u32 sequence;

	if (selected >= PS_LOG_MIRROR_SLOT_COUNT)
		return;
	sequence = READ_ONCE(ps_log_mirror_slots[selected].sequence);
	if (!(sequence & 1U))
		WRITE_ONCE(ps_log_mirror_slots[selected].sequence, sequence + 1U);
}

void pstore_screen_log_set_gap_count_for_test(int count)
{
	atomic_set(&ps_log_mirror_gap_count, count);
}

int pstore_screen_log_gap_count_for_test(void)
{
	return atomic_read(&ps_log_mirror_gap_count);
}

void pstore_screen_log_arm_invalidate_interleave_for_test(void)
{
	WRITE_ONCE(ps_log_mirror_invalidate_interleave_for_test, true);
}

void pstore_screen_log_arm_freeze_interleave_for_test(void)
{
	WRITE_ONCE(ps_log_mirror_freeze_interleave_for_test, true);
}
#endif

#else

static void ps_log_mirror_state_init(void)
{
	memset(&ps_log_frozen_mirror, 0, sizeof(ps_log_frozen_mirror));
	ps_log_mirror_reset(&ps_log_frozen_mirror,
			    PS_MIRROR_SOURCE_ABSENT);
}

int pstore_screen_log_freeze_mirror_internal(struct ps_log_mirror_image *destination)
{
	if (!destination)
		return -EINVAL;
	ps_log_mirror_reset(destination, PS_MIRROR_SOURCE_ABSENT);
	return 0;
}

#endif

void pstore_screen_log_panic_capture(const char *description, size_t len,
				     bool invalid)
{
	if (cmpxchg(&ps_log_panic_description_state, 0U, 1U) != 0U)
		return;
	if ((!description && len) || len >= PS_LOG_PANIC_DESCRIPTION_BYTES)
		invalid = true;
	if (!invalid && len)
		memcpy(ps_log_panic_description, description, len);
	ps_log_panic_description_len = invalid ? 0U : (u32)len;
	ps_log_panic_description_invalid = invalid;
	/* Publish description bytes and validity before the panic dumper. */
	smp_store_release(&ps_log_panic_description_state, 2U);
}

static u64 ps_log_kmsg_cursor(void *ctx)
{
	struct kmsg_dumper *dumper = ctx;

	return READ_ONCE(dumper->cur_seq);
}

static int ps_log_kmsg_get_line(void *ctx, u32 syslog, u8 *line,
				u32 capacity, u32 *len)
{
	struct kmsg_dumper *dumper = ctx;
	size_t line_len = 0U;
	bool got;

	if (syslog != 1U)
		return -EINVAL;
	got = kmsg_dump_get_line_nolock(dumper, true, (char *)line,
					capacity, &line_len);
	if (line_len > 0xffffffffU)
		return -EOVERFLOW;
	*len = (u32)line_len;
	return got ? 1 : 0;
}

static const struct ps_log_reader_ops ps_log_kmsg_ops = {
	.cursor = ps_log_kmsg_cursor,
	.get_line = ps_log_kmsg_get_line,
};

int pstore_screen_log_snapshot_internal(const u8 **data, u32 *data_len,
					struct ps_pslc_summary *summary)
{
	if (!data || !data_len)
		return -EINVAL;
	/* Pair with the dumper's publication of the complete PSLC buffer. */
	if (!smp_load_acquire(&ps_log_container_valid))
		return -ENODATA;
	*data = ps_log_container;
	*data_len = READ_ONCE(ps_log_container_len);
	if (summary)
		*summary = ps_log_container_summary;
	return 0;
}

static void ps_log_kmsg_dump(struct kmsg_dumper *dumper,
			     enum kmsg_dump_reason reason)
{
	struct ps_log_capture_result capture;
	u64 start_seq;
	u64 end_seq;
	u32 description_len;
	u32 output_len;
	int ret;

	if (reason != KMSG_DUMP_PANIC ||
	    cmpxchg(&ps_log_dump_state, 0U, 1U) != 0U)
		return;
	WRITE_ONCE(ps_log_container_valid, false);
	if (!pstore_screen_printk_ring_ready())
		goto out_invalid;
	/* Pair with panic_capture before consuming its private description. */
	if (smp_load_acquire(&ps_log_panic_description_state) != 2U ||
	    READ_ONCE(ps_log_panic_description_invalid))
		goto out_invalid;
	description_len = READ_ONCE(ps_log_panic_description_len);
	pstore_screen_printk_snapshot_window(dumper);
	start_seq = READ_ONCE(dumper->cur_seq);
	end_seq = READ_ONCE(dumper->next_seq);
	ret = ps_log_capture_frozen_window(start_seq, end_seq, &ps_log_kmsg_ops,
					   dumper, ps_log_kmsg_scratch,
					   PS_LOG_SCRATCH_BYTES, &capture);
	if (ret)
		goto out_invalid;
	ret = pstore_screen_log_freeze_mirror_internal(&ps_log_frozen_mirror);
	if (ret)
		goto out_invalid;
	ret = ps_log_build_pslc(ps_log_panic_description, description_len,
				&capture, &ps_log_frozen_mirror,
				ps_log_segments, PS_LOG_MAX_SEGMENTS,
				ps_log_container, PS_LOG_CONTAINER_BYTES,
				&output_len, &ps_log_container_summary);
	if (ret)
		goto out_invalid;
	WRITE_ONCE(ps_log_container_len, output_len);
	/* Publish the complete container and summary before snapshot readers. */
	smp_store_release(&ps_log_container_valid, true);
	WRITE_ONCE(ps_log_dump_state, 2U);
	return;

out_invalid:
	WRITE_ONCE(ps_log_dump_state, 3U);
}

static struct kmsg_dumper ps_log_dumper = {
	.dump = ps_log_kmsg_dump,
	.max_reason = KMSG_DUMP_PANIC,
};

static int __init pstore_screen_log_init(void)
{
	ps_log_mirror_state_init();
	return kmsg_dump_register(&ps_log_dumper);
}
early_initcall(pstore_screen_log_init);

#endif
