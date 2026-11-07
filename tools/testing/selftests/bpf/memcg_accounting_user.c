// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <linux/bpf.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef __NR_bpf
#define __NR_bpf 280 /* ARM64 */
#endif

static int create(unsigned type, unsigned flags)
{
	union bpf_attr attr = {};
	attr.map_type = type;
	attr.key_size = 4;
	attr.value_size = 64;
	attr.max_entries = 1024;
	attr.map_flags = flags;
	return syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof(attr));
}

int main(void)
{
	struct rlimit limit = {0, 0};
	int maps[4] = {-1, -1, -1, -1}, ret = 1;
	unsigned types[] = {BPF_MAP_TYPE_ARRAY, BPF_MAP_TYPE_PERCPU_ARRAY,
		BPF_MAP_TYPE_HASH, BPF_MAP_TYPE_PERCPU_HASH};
	unsigned key;
	long cpus = sysconf(_SC_NPROCESSORS_CONF);
	void *value;
	union bpf_attr attr = {};

	if (cpus <= 0)
		return 1;
	value = calloc(cpus, 64);
	if (!value)
		return 1;
	if (setrlimit(RLIMIT_MEMLOCK, &limit)) {
		perror("setrlimit");
		goto out;
	}
	printf("PID=%ld: creating maps with RLIMIT_MEMLOCK=0\n", (long)getpid());
	for (unsigned i = 0; i < 4; i++) {
		maps[i] = create(types[i], i >= 2 ? BPF_F_NO_PREALLOC : 0);
		if (maps[i] < 0) {
			perror("BPF_MAP_CREATE");
			goto out;
		}
	}
	puts("Maps created. Inspect creator cgroup usage, then move this PID to another cgroup.");
	puts("Press Enter to populate the dynamic maps; charges should stay with the creator.");
	getchar();
	for (unsigned i = 2; i < 4; i++) {
		for (key = 0; key < 1024; key++) {
			memset(&attr, 0, sizeof(attr));
			attr.map_fd = maps[i];
			attr.key = (uintptr_t)&key;
			attr.value = (uintptr_t)value;
			if (syscall(__NR_bpf, BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr))) {
				perror("BPF_MAP_UPDATE_ELEM");
				goto out;
			}
		}
	}
	puts("Maps populated. Inspect both cgroups. Press Enter to close maps and exit.");
	getchar();
	ret = 0;
out:
	for (unsigned i = 0; i < 4; i++)
		if (maps[i] >= 0)
			close(maps[i]);
	free(value);
	return ret;
}
