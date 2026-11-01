/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PSTORE_SCREEN_LOG_INTERNAL_H
#define _PSTORE_SCREEN_LOG_INTERNAL_H

#include "pstore_screen_core.h"

#ifndef PSTORE_SCREEN_HOST
#include <linux/pstore_screen_log.h>
#else
#define PSTORE_SCREEN_MAX_FORMATTED_RECORD_BYTES 65536U
#endif

#define PS_LOG_TAIL_BYTES 262144U
#define PS_MAX_FORMATTED_RECORD_BYTES \
	PSTORE_SCREEN_MAX_FORMATTED_RECORD_BYTES
#define PS_LOG_GUARD_BYTES 1U
#define PS_LOG_GET_LINE_BYTES \
	(PS_MAX_FORMATTED_RECORD_BYTES + PS_LOG_GUARD_BYTES)
#define PS_LOG_SCRATCH_BYTES 327681U
#define PS_LOG_GUARD_VALUE 0xa5U

#define PS_LOG_PANIC_DESCRIPTION_BYTES 1024U
#define PS_LOG_MAX_PSTORE_RECORDS 128U
#define PS_LOG_PSTORE_BYTES 262144U
#define PS_LOG_MAX_SEGMENTS (2U + PS_LOG_MAX_PSTORE_RECORDS)
#define PS_LOG_CONTAINER_BYTES 531968U
#define PS_LOG_MIRROR_SLOT_COUNT 3U
#define PS_LOG_SLOT_NONE 0xffffffffU

struct ps_log_reader_ops {
	u64 (*cursor)(void *ctx);
	int (*get_line)(void *ctx, u32 syslog, u8 *line, u32 capacity,
			u32 *len);
	void (*checkpoint)(void *ctx);
};

struct ps_log_capture_result {
	const u8 *payload;
	u32 payload_len;
	u32 current_total_len;
	u64 start_seq;
	u64 end_seq;
	u32 valid;
};

struct ps_log_pstore_input {
	u32 pstore_type;
	u32 original_compressed;
	u32 remains_compressed;
	u32 codec;
	u64 record_id;
	u64 timestamp_sec;
	u32 timestamp_nsec;
	u32 count;
	u32 reason;
	u32 part;
	const u8 *payload;
	u32 payload_len;
	const u8 *ecc;
	u32 ecc_len;
};

struct ps_log_mirror_record {
	u32 flags;
	u32 ordinal;
	u32 pstore_type;
	u64 record_id;
	u64 timestamp_sec;
	u32 timestamp_nsec;
	u32 payload_offset;
	u32 payload_len;
	u32 ecc_offset;
	u32 ecc_len;
	u32 count;
	u32 reason;
	u32 part;
};

struct ps_log_mirror_image {
	u32 source;
	u32 events;
	u32 valid;
	u32 total_count;
	u32 record_count;
	u32 arena_len;
	struct ps_log_mirror_record *records;
	u32 record_capacity;
	u8 *arena;
	u32 arena_capacity;
};

int ps_log_checked_add_u32(u32 left, u32 right, u32 *out);
int ps_log_capture_frozen_window(u64 start_seq, u64 end_seq,
				 const struct ps_log_reader_ops *ops, void *ctx,
				 u8 *scratch, u32 scratch_len,
				 struct ps_log_capture_result *result);

void ps_log_mirror_reset(struct ps_log_mirror_image *image, u32 source);
int ps_log_mirror_append(struct ps_log_mirror_image *image,
			 const struct ps_log_pstore_input *input);
int ps_log_mirror_finish(struct ps_log_mirror_image *image, u32 events);
int ps_log_mirror_validate(const struct ps_log_mirror_image *image);

int ps_log_build_pslc(const u8 *description, u32 description_len,
		      const struct ps_log_capture_result *capture,
		      const struct ps_log_mirror_image *mirror,
		      struct ps_pslc_segment_input *segments,
		      u32 segment_capacity, u8 *out, u32 out_capacity,
		      u32 *out_len, struct ps_pslc_summary *summary);

#ifndef PSTORE_SCREEN_HOST
int pstore_screen_log_snapshot_internal(const u8 **data, u32 *data_len,
					struct ps_pslc_summary *summary);
int pstore_screen_log_freeze_mirror_internal(struct ps_log_mirror_image *destination);

#if defined(CONFIG_PSTORE_SCREEN_LOG_KUNIT_TEST) && \
	defined(CONFIG_PSTORE_SCREEN_PSTORE_MIRROR)
void pstore_screen_log_reset_mirror_for_test(void);
void pstore_screen_log_mirror_indices_for_test(u32 *active, u32 *previous,
					       u32 *inactive);
void pstore_screen_log_corrupt_active_sequence_for_test(void);
void pstore_screen_log_set_gap_count_for_test(int count);
int pstore_screen_log_gap_count_for_test(void);
void pstore_screen_log_arm_invalidate_interleave_for_test(void);
void pstore_screen_log_arm_freeze_interleave_for_test(void);
#endif
#endif

#endif
