#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the production per-CPU objcg hooks with host stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "mm/percpu.c").read_text()
start = source.index("static bool pcpu_memcg_pre_alloc_hook")
end = source.index("#else\nstatic bool pcpu_memcg_pre_alloc_hook", start)
hooks = source[start:end]

program = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define __GFP_ACCOUNT 128
#define PCPU_MIN_ALLOC_SHIFT 2
#define likely(x) (x)
typedef unsigned gfp_t;
struct obj_cgroup { int refs; long bytes; } owner = { .refs = 1 };
struct pcpu_chunk { struct obj_cgroup *obj_cgroups[16]; } chunk;
static bool enabled = true, fail_charge = false;
static long charged, stats;
static bool memcg_kmem_enabled(void) { return enabled; }
static int num_possible_cpus(void) { return 8; }
static struct obj_cgroup *get_obj_cgroup_from_current(void) {
 owner.refs++; return &owner;
}
static int obj_cgroup_charge(struct obj_cgroup *o, gfp_t g, size_t n) {
 if (fail_charge) return -1;
 charged += n; return 0;
}
static void obj_cgroup_uncharge(struct obj_cgroup *o, size_t n) {
 assert(charged >= (long)n); charged -= n;
}
static void obj_cgroup_put(struct obj_cgroup *o) {
 assert(o->refs > 1); o->refs--;
}
static void mod_objcg_percpu_state(struct obj_cgroup *o, long n) {
 o->bytes += n; stats += n;
}
""" + hooks + r"""
int main(void) {
 struct obj_cgroup *o = NULL;
 assert(pcpu_memcg_pre_alloc_hook(16, __GFP_ACCOUNT, &o));
 assert(o == &owner && charged == 128 && owner.refs == 2);
 pcpu_memcg_post_alloc_hook(o, &chunk, 4, 16);
 assert(chunk.obj_cgroups[1] == &owner && stats == 128);
 pcpu_memcg_free_hook(&chunk, 4, 16);
 assert(!charged && !stats && !owner.bytes && owner.refs == 1);
 assert(!chunk.obj_cgroups[1]);

 o = NULL; fail_charge = true;
 assert(!pcpu_memcg_pre_alloc_hook(16, __GFP_ACCOUNT, &o));
 assert(!o && !charged && owner.refs == 1);
 fail_charge = false;

 assert(pcpu_memcg_pre_alloc_hook(16, __GFP_ACCOUNT, &o));
 pcpu_memcg_post_alloc_hook(o, NULL, 0, 16);
 assert(!charged && !stats && owner.refs == 1);

 enabled = false; o = NULL;
 assert(pcpu_memcg_pre_alloc_hook(16, __GFP_ACCOUNT, &o));
 assert(!o && !charged);
 puts("PASS: byte charge, ownership, free, charge failure, rollback, disabled accounting");
 return 0;
}
"""

with tempfile.TemporaryDirectory() as temp:
    test = Path(temp) / "test.c"
    test.write_text(program)
    subprocess.run(["cc", str(test), "-o", str(Path(temp) / "test")], check=True)
    subprocess.run([str(Path(temp) / "test")], check=True)
