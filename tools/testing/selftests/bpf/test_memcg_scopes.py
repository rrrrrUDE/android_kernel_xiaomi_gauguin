#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute real map allocation scopes with process/IRQ and failure stubs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[4]
h=(root/'include/linux/sched/mm.h').read_text();s=(root/'kernel/bpf/syscall.c').read_text()
a=h.index('static inline struct mem_cgroup *\nset_active_memcg');b=h.index('\n}',a)+2
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define __percpu
#define __GFP_ACCOUNT 128
#define in_task() (!irq)
#define this_cpu_read(x) (x)
#define this_cpu_write(x,v) ((x)=(v))
typedef unsigned gfp_t;
struct mem_cgroup { int id; };
struct task { struct mem_cgroup *active_memcg; } task;
#define current (&task)
static bool irq,fail;
static struct mem_cgroup *int_active_memcg,*seen;
struct bpf_map { struct mem_cgroup *memcg; };
'''+h[a:b]+r'''
static void *allocation(gfp_t flags) { assert(flags&__GFP_ACCOUNT); seen=irq?int_active_memcg:task.active_memcg; return fail?NULL:(void *)1; }
#define kmalloc_node(n,f,node) allocation(f)
#define kzalloc(n,f) allocation(f)
#define __alloc_percpu_gfp(n,a,f) allocation(f)
'''
for name in ['bpf_map_kmalloc_node','bpf_map_kzalloc','bpf_map_alloc_percpu']:
 a=s.index(('void __percpu *' if name=='bpf_map_alloc_percpu' else 'void *')+name+'(');b=s.index('\n}',a)+2;c+=s[a:b]
c+=r'''
int main(void) {
 struct mem_cgroup owner={1},previous={2};struct bpf_map map={&owner};
 for(irq=0;;irq=1) {
  task.active_memcg=&previous;int_active_memcg=&previous;
  for(int i=0;i<2;i++) {
   fail=i;
   assert((bpf_map_kmalloc_node(&map,16,0,0)!=NULL)==!fail);
   assert(seen==&owner);
   assert((bpf_map_kzalloc(&map,16,0)!=NULL)==!fail);
   assert((bpf_map_alloc_percpu(&map,16,8,0)!=NULL)==!fail);
   assert(task.active_memcg==&previous && int_active_memcg==&previous);
  }
  if(irq)break;
 }
 puts("PASS: owner-directed process/IRQ allocation, nested scope restoration and allocation failures");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(c);subprocess.run(['cc',str(p),'-o',d+'/test'],check=True);subprocess.run([d+'/test'],check=True)
