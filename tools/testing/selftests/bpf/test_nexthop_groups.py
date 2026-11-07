#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run production group weighting/path selection with neighbour stubs."""
from pathlib import Path
import subprocess
import tempfile
src=(Path(__file__).resolve().parents[4]/'net/ipv4/nexthop.c').read_text()
def fn(name):
 a=src.index(name);b=src.index('{',a);d=1;e=b+1
 while d:
  d+=(src[e]=='{')-(src[e]=='}');e+=1
 return src[a:e]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
typedef uint16_t u16;typedef uint64_t u64;
#define AF_INET 2
#define AF_INET6 10
#define NULL ((void *)0)
#define rcu_dereference(p) (p)
#define atomic_read(p) (*(p))
#define atomic_set(p,v) (*(p)=(v))
#define DIV_ROUND_CLOSEST_ULL(n,d) (((n)+(d)/2)/(d))
struct fib_nh {bool good;};struct fib6_nh {bool good;};
struct nh_info {bool fdb_nh;int family;struct fib_nh fib_nh;struct fib6_nh fib6_nh;};
struct nexthop;struct nh_group;
struct nh_grp_entry {struct nexthop *nh;unsigned weight;struct {int upper_bound;} hthr;};
struct nh_res_bucket {struct nh_grp_entry *nh_entry;unsigned used;};
struct nh_res_table {u16 num_nh_buckets;struct nh_res_bucket nh_buckets[4];};
struct nh_group {int num_nh;bool hash_threshold,resilient;struct nh_res_table *res_table;struct nh_grp_entry nh_entries[3];};
struct nexthop {bool is_group;struct nh_info *nh_info;struct nh_group *nh_grp;};
static bool ipv4_good_nh(const struct fib_nh *n){return n->good;}
static bool ipv6_good_nh(const struct fib6_nh *n){return n->good;}
static void nh_res_bucket_set_busy(struct nh_res_bucket *b){++b->used;}
'''
for name in ['static struct nexthop *nexthop_select_path_hthr(', 'static struct nexthop *nexthop_select_path_res(', 'struct nexthop *nexthop_select_path(', 'static void nh_hthr_group_rebalance(']:code+=fn(name)+'\n'
code+=r'''
int main(void){
 struct nh_info info[3]={{.family=AF_INET,.fib_nh={true}}, {.family=AF_INET6,.fib6_nh={true}}, {.family=AF_INET,.fib_nh={true}}};
 struct nexthop n[3]={{.nh_info=&info[0]}, {.nh_info=&info[1]}, {.nh_info=&info[2]}};
 struct nh_group g={.num_nh=3,.hash_threshold=true};
 struct nexthop group={.is_group=true,.nh_grp=&g};
 for(int i=0;i<3;i++){g.nh_entries[i].nh=&n[i];g.nh_entries[i].weight=i+1;}
 nh_hthr_group_rebalance(&g);
 int first=g.nh_entries[0].hthr.upper_bound,second=g.nh_entries[1].hthr.upper_bound;
 assert(first>0 && second>first && g.nh_entries[2].hthr.upper_bound==INT_MAX);
 assert(nexthop_select_path(&n[0],INT_MAX)==&n[0]);
 assert(nexthop_select_path(&group,0)==&n[0]);
 assert(nexthop_select_path(&group,first)==&n[0]);
 assert(nexthop_select_path(&group,first+1)==&n[1]);
 assert(nexthop_select_path(&group,second+1)==&n[2]);
 info[0].fib_nh.good=false;assert(nexthop_select_path(&group,0)==&n[1]);
 info[1].fib6_nh.good=false;info[2].fib_nh.good=false;
 assert(nexthop_select_path(&group,0)==&n[0]);
 info[1].fdb_nh=true;assert(nexthop_select_path(&group,0)==&n[1]);
 struct nh_res_table t={.num_nh_buckets=4};g.hash_threshold=false;g.resilient=true;g.res_table=&t;
 for(int i=0;i<4;i++)t.nh_buckets[i].nh_entry=&g.nh_entries[i%3];
 for(int h=0;h<100;h++)assert(nexthop_select_path(&group,h)==&n[(h%4)%3]);
 for(int i=0;i<4;i++)assert(t.nh_buckets[i].used==25);
 puts("PASS: production group weight boundaries, neighbour fallback, FDB and resilient bucket selection/activity");
}
'''
with tempfile.TemporaryDirectory(prefix='nh-groups-') as temp:
 s=Path(temp)/'test.c';b=Path(temp)/'test';s.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Werror',str(s),'-o',str(b)],check=True)
 subprocess.run([str(b)],check=True)
