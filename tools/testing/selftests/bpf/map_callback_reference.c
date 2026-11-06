// SPDX-License-Identifier: GPL-2.0
/* Default must load. Build with -DBAD_RELEASE or -DLEAK_REFERENCE to reject. */
#include <linux/bpf.h>
#include "bpf_helpers.h"
#define __uint(name, val) int (*name)[val]
#define __type(name, val) val *name
#define NOINLINE __attribute__((noinline))
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} array SEC(".maps");
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 4096);
} ring SEC(".maps");
static long (*for_each)(void *, void *, void *, __u64) =
	(void *)BPF_FUNC_for_each_map_elem;
static void *(*reserve)(void *, __u64, __u64) = (void *)BPF_FUNC_ringbuf_reserve;
static void (*discard)(void *, __u64) = (void *)BPF_FUNC_ringbuf_discard;

static NOINLINE void *get_reference(void)
{
	return reserve(&ring, 8, 0);
}
static NOINLINE void put_reference(void *ptr)
{
	discard(ptr, 0);
}
static long cb(void *map, __u32 *key, __u64 *value, void *ctx)
{
#ifdef BAD_RELEASE
	/* A descendant must not consume a reference inherited from outside
	 * this callback; the caller may still use it after the helper returns.
	 */
	put_reference(*(void **)ctx);
#else
	void *ptr = get_reference();

	if (ptr) {
#ifndef LEAK_REFERENCE
		put_reference(ptr);
#endif
	}
#endif
	return 0;
}
SEC("classifier")
int map_callback_reference(struct __sk_buff *skb)
{
#ifdef BAD_RELEASE
	/* A stack slot carries the caller-owned ringbuf pointer. */
	void *ptr = get_reference();
	if (ptr) {
		for_each(&array, cb, &ptr, 0);
		put_reference(ptr);
	}
#else
	for_each(&array, cb, 0, 0);
#endif
	return 0;
}
char _license[] SEC("license") = "GPL";
