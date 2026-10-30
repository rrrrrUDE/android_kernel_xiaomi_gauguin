// SPDX-License-Identifier: GPL-2.0
/* Load this object on the backported kernel and run the classifier once.
 * The verifier must accept both callbacks; the result map records counts.
 */
#include <linux/bpf.h>
#include "bpf_helpers.h"

#define __uint(name, val) int (*name)[val]
#define __type(name, val) val *name

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 3);
	__type(key, __u32);
	__type(value, __u64);
} array SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 3);
	__type(key, __u32);
	__type(value, __u64);
} hash SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 2);
	__type(key, __u32);
	__type(value, __u64);
} result SEC(".maps");

static long (*for_each_map_elem)(void *map, void *callback, void *ctx,
				 unsigned long long flags) =
	(void *)BPF_FUNC_for_each_map_elem;

static long count_elem(void *map, __u32 *key, __u64 *value, __u64 *count)
{
	(*count)++;
	return 0;
}

SEC("classifier")
int check_map_callbacks(struct __sk_buff *skb)
{
	__u64 count = 0, *out;
	__u32 key = 0;

	for_each_map_elem(&array, count_elem, &count, 0);
	out = bpf_map_lookup_elem(&result, &key);
	if (out)
		*out = count;

	count = 0;
	for_each_map_elem(&hash, count_elem, &count, 0);
	key = 1;
	out = bpf_map_lookup_elem(&result, &key);
	if (out)
		*out = count;
	return 0;
}

char _license[] SEC("license") = "GPL";
