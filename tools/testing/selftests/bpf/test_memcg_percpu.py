#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise production per-CPU charging wrapper, rollback and free with host stubs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[4]
s=(root/'mm/percpu.c').read_text();a=s.index('struct pcpu_memcg_record {');b=s.index('\n#else',a)
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define __percpu
typedef unsigned gfp_t;typedef uint64_t u64;
#define __GFP_ACCOUNT 128
#define PAGE_SIZE 4096
#define PCPU_MIN_ALLOC_SIZE 4
#define PCPU_MIN_UNIT_SIZE 32768
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define DIV_ROUND_UP_ULL(x,y) (((x)+(y)-1)/(y))
#define is_power_of_2(x) ((x)&&!((x)&((x)-1)))
#define num_possible_cpus() 8
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define DEFINE_HASHTABLE(n,b) int n
#define DEFINE_SPINLOCK(n) int n
#define spin_lock_irqsave(l,f) ((void)0)
#define spin_unlock_irqrestore(l,f) ((void)0)
struct hlist_node { int unused; };
struct mem_cgroup { int id; } owner;
static bool enabled=true,fail_charge,fail_metadata,fail_alloc;
static unsigned charged,uncharge_calls;
struct pcpu_memcg_record;
static struct pcpu_memcg_record *head;
#define hash_add(t,n,k) (head=(struct pcpu_memcg_record *)(n))
#define hash_del(n) (head=NULL)
#define hash_for_each_possible(t,r,n,k) for((r)=head;(r);(r)=NULL)
static bool memcg_kmem_enabled(void) { return enabled; }
static struct mem_cgroup *memcg_kmem_charge_percpu(gfp_t f,unsigned n) { if(fail_charge)return (void *)-12;charged+=n;return &owner; }
static void memcg_kmem_uncharge_percpu(struct mem_cgroup *m,unsigned n) { assert(m==&owner && charged>=n);charged-=n;uncharge_calls++; }
static void *kmalloc(size_t n,gfp_t f) { return fail_metadata?NULL:malloc(n); }
#define kfree(p) free(p)
static void *pcpu_alloc_unaccounted(size_t n,size_t a,bool r,gfp_t f) { return fail_alloc?NULL:malloc(n*8); }
'''+s[a:b]+r'''
int main(void) {
 for(unsigned mode=0;mode<4;mode++) {
  charged=uncharge_calls=0;
  fail_charge=mode==1;fail_metadata=mode==2;fail_alloc=mode==3;
  void *p=pcpu_alloc(64,8,false,__GFP_ACCOUNT);
  if(mode) { assert(!p && !charged && !head);assert(uncharge_calls==(mode>=2)); }
  else { assert(p && charged==1 && head);pcpu_memcg_free(p);assert(!charged&&!head&&uncharge_calls==1);free(p); }
 }
 fail_charge=fail_metadata=fail_alloc=false;
 assert(!pcpu_alloc(0,8,false,__GFP_ACCOUNT));
 assert(!pcpu_alloc(32769,8,false,__GFP_ACCOUNT));
 assert(!pcpu_alloc(16,6,false,__GFP_ACCOUNT));
 for(unsigned align=0;align<4;align++) {
  void *p=pcpu_alloc(16,align,false,__GFP_ACCOUNT);
  assert(p&&charged==1);pcpu_memcg_free(p);assert(!charged);free(p);
 }
 void *p=pcpu_alloc(1024,8,false,__GFP_ACCOUNT);
 assert(p&&charged==2);pcpu_memcg_free(p);assert(!charged);free(p);
 enabled=false;p=pcpu_alloc(64,8,false,__GFP_ACCOUNT);assert(p&&!head&&!charged);pcpu_memcg_free(p);free(p);
 puts("PASS: production per-CPU charge/free, allocation and metadata rollback, invalid sizes and disabled accounting");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(c);subprocess.run(['cc',str(p),'-o',d+'/test'],check=True);subprocess.run([d+'/test'],check=True)
