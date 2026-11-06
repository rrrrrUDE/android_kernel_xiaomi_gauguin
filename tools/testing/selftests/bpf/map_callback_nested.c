// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include "bpf_helpers.h"

#define __uint(name, val) int (*name)[val]
#define __type(name, val) val *name
#define NOINLINE __attribute__((noinline))

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 3);
	__type(key, __u32);
	__type(value, __u64);
} outer SEC(".maps"), inner SEC(".maps");
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} result SEC(".maps");

static long (*for_each)(void *, void *, void *, __u64) =
	(void *)BPF_FUNC_for_each_map_elem;

/* Return >1 deliberately: ordinary subprograms must not be mistaken for
 * callback entry frames and subjected to callback's [0,1] return check.
 */
static NOINLINE long add(__u64 *count, __u32 *key)
{
	volatile long ret = 7;

	*count += *key + 1;
	return ret;
}

static long inner_cb(void *map, __u32 *key, __u64 *value, __u64 *count)
{
	return add(count, key) == 7 ? 0 : 1;
}

static NOINLINE long nested(__u64 *count)
{
	return for_each(&inner, inner_cb, count, 0);
}

static long outer_cb(void *map, __u32 *key, __u64 *value, __u64 *count)
{
	/* callback -> ordinary subprogram -> callback -> ordinary subprogram */
	return nested(count) == 3 ? 0 : 1;
}

SEC("classifier")
int map_callback_nested(struct __sk_buff *skb)
{
	__u64 count = 0, *out;
	__u32 key = 0;

	for_each(&outer, outer_cb, &count, 0);
	out = bpf_map_lookup_elem(&result, &key);
	if (out)
		*out = count; /* 3 * (1 + 2 + 3) = 18 */
	return 0;
}
char _license[] SEC("license") = "GPL";
