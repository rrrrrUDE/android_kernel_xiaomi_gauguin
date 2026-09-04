/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PSTORE_SCREEN_LOG_H
#define _LINUX_PSTORE_SCREEN_LOG_H

#include <linux/types.h>

#define PSTORE_SCREEN_MIN_DESCRIPTORS 524288U
#define PSTORE_SCREEN_MAX_FORMATTED_RECORD_BYTES 65536U

#define PSTORE_SCREEN_MIRROR_EVENT_SCAN_INCOMPLETE 0x08U
#define PSTORE_SCREEN_MIRROR_EVENT_BACKEND_READ_FAILURE 0x10U

struct pstore_record;
struct kmsg_dumper;

#if IS_ENABLED(CONFIG_PSTORE_SCREEN_LOG_CAPTURE)
bool pstore_screen_printk_ring_ready(void);
void pstore_screen_printk_snapshot_window(struct kmsg_dumper *dumper);
void pstore_screen_log_panic_capture(const char *description, size_t len,
				     bool invalid);
#if defined(CONFIG_PSTORE_SCREEN_LOG_KUNIT_TEST)
void pstore_screen_printk_clear_for_test(void);
u64 pstore_screen_printk_next_seq_for_test(void);
int pstore_screen_printk_record_lengths_for_test(u64 seq, char *buf,
						 size_t capacity,
						 size_t *formatted_len,
						 size_t *required_len);
#endif
#else
static inline bool pstore_screen_printk_ring_ready(void)
{
	return false;
}

static inline void pstore_screen_printk_snapshot_window(struct kmsg_dumper *dumper)
{
}

static inline void pstore_screen_log_panic_capture(const char *description,
						   size_t len, bool invalid)
{
}
#endif

#if IS_ENABLED(CONFIG_PSTORE_SCREEN_PSTORE_MIRROR)
void pstore_screen_log_mirror_scan_begin(void);
void pstore_screen_log_mirror_record(const struct pstore_record *record,
				     bool was_compressed,
				     const char *codec_name);
void pstore_screen_log_mirror_scan_end(u32 events);
void pstore_screen_log_mirror_invalidate(void);
#else
static inline void pstore_screen_log_mirror_scan_begin(void)
{
}

static inline void pstore_screen_log_mirror_record(const struct pstore_record *record,
						   bool was_compressed,
						   const char *codec_name)
{
}

static inline void pstore_screen_log_mirror_scan_end(u32 events)
{
}

static inline void pstore_screen_log_mirror_invalidate(void)
{
}
#endif

#endif
