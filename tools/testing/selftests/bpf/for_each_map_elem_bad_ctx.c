// SPDX-License-Identifier: GPL-2.0
/* Loading this object must fail: callback context is a map value pointer. */
#include <linux/bpf.h>
#include "bpf_helpers.h"

#define __uint(name, val) int (*name)[val]
#define __type(name, val) val *name

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} array SEC(".maps");

static long (*for_each_map_elem)(void *map, void *callback, void *ctx,
				 unsigned long long flags) =
	(void *)BPF_FUNC_for_each_map_elem;

static long count_elem(void *map, __u32 *key, __u64 *value, void *ctx)
{
	return 0;
}

SEC("classifier")
int reject_map_value_context(struct __sk_buff *skb)
{
	__u32 key = 0;
	void *value = bpf_map_lookup_elem(&array, &key);

	if (value)
		for_each_map_elem(&array, count_elem, value, 0);
	return 0;
}

char _license[] SEC("license") = "GPL";
