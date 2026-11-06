// SPDX-License-Identifier: GPL-2.0
/* Read-only rodata must work for input helpers and fail as output memory. */
#include <linux/bpf.h>
#include "bpf_helpers.h"
const volatile char input[4] = "123";
static long (*parse)(const char *, __u32, __u64, long *) =
	(void *)BPF_FUNC_strtol;
SEC("classifier")
int readonly_helper_test(struct __sk_buff *skb)
{
#ifdef BAD_WRITE
	return bpf_get_current_comm((void *)input, sizeof(input));
#else
	long result = 0;
	long ret = parse((const char *)input, sizeof(input), 0, &result);

	return ret > 0 && result == 123 ? 0 : 1;
#endif
}
char _license[] SEC("license") = "GPL";
