// SPDX-License-Identifier: GPL-2.0
/* Runtime regression for callback tail calls and shared counter exhaustion.
 * Do not treat an object build as a device execution pass.
 */
#include <linux/bpf.h>
#include "bpf_helpers.h"
#define __uint(name, val) int (*name)[val]
#define __type(name, val) val *name

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} array SEC(".maps"), result SEC(".maps");
struct {
	__uint(type, BPF_MAP_TYPE_PROG_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} targets SEC(".maps");
struct context {
	struct __sk_buff *skb;
};
static long (*for_each)(void *, void *, void *, __u64) =
	(void *)BPF_FUNC_for_each_map_elem;

static long cb(void *map, __u32 *key, __u64 *value, struct context *ctx)
{
	bpf_tail_call(ctx->skb, &targets, 0);
	return 0;
}
SEC("classifier/entry")
int callback_tail_entry(struct __sk_buff *skb)
{
	struct context ctx = { .skb = skb };

	for_each(&array, cb, &ctx, 0);
	return 0;
}
SEC("classifier/target")
int callback_tail_target(struct __sk_buff *skb)
{
	__u32 key = 0;
	__u64 *out = bpf_map_lookup_elem(&result, &key);
	struct context ctx = { .skb = skb };

	if (out)
		__sync_fetch_and_add(out, 1);
	/* Re-enter the C map helper and callback after every successful tail
	 * call. A broken per-callback counter reset makes this unbounded.
	 */
	for_each(&array, cb, &ctx, 0);
	return 0;
}
char _license[] SEC("license") = "GPL";
