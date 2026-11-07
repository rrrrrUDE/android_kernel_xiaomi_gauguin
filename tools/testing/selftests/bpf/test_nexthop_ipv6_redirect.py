#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute production member recovery for normal and redirected IPv6 dsts."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[4]
s=(root/'net/ipv6/route.c').read_text();a=s.index('static struct fib6_nh *rt6_nh_for_dst(');b=s.index('static bool rt6_nh_eligible(',a)
code=r'''
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define RTF_GATEWAY 2
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct in6_addr {int value;};
struct net_device {int id;};
struct fib_nh_common {int unused;};
struct fib6_nh {struct fib_nh_common nh_common;struct net_device *fib_nh_dev;struct in6_addr fib_nh_gw6;};
struct fib6_info {void *nh;struct fib6_nh fib6_nh;};
struct rt6_info {struct {struct net_device *dev;} dst;int rt6i_flags;struct in6_addr rt6i_gateway;struct {struct in6_addr addr;} rt6i_dst,rt6i_src;};
static struct fib6_nh members[2];
static struct rt6_info *exceptions[2];
static int nexthop_num_path(void *n) {return 2;}
static struct fib_nh_common *nexthop_fib_nhc(void *n,int i) {return &members[i].nh_common;}
static int ipv6_addr_equal(const struct in6_addr *a,const struct in6_addr *b) {return a->value==b->value;}
static struct rt6_info *rt6_find_cached_rt_nh(struct fib6_info *f,struct fib6_nh *n,const struct in6_addr *d,const struct in6_addr *s) {return n==&members[0]?exceptions[0]:exceptions[1];}
'''+s[a:b]+r'''
int main(void) {
 struct net_device dev={1};struct fib6_info route={.nh=members};
 members[0].fib_nh_dev=members[1].fib_nh_dev=&dev;
 members[0].fib_nh_gw6.value=1;members[1].fib_nh_gw6.value=2;
 struct rt6_info dst={.dst={&dev},.rt6i_flags=RTF_GATEWAY,.rt6i_gateway={2}};
 assert(rt6_nh_for_dst(&route,&dst)==&members[1]);
 dst.rt6i_gateway.value=3;exceptions[1]=&dst;
 assert(rt6_nh_for_dst(&route,&dst)==&members[1]);
 dst.rt6i_gateway.value=1;assert(rt6_nh_for_dst(&route,&dst)==&members[1]);
 dst.rt6i_gateway.value=3;exceptions[1]=NULL;assert(!rt6_nh_for_dst(&route,&dst));
 puts("PASS: production IPv6 gateway member matching and redirected-exception ownership recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='nh-v6-redirect-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Werror',str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
