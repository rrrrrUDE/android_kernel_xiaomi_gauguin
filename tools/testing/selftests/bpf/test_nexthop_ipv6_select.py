#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise production IPv6 object selection and result isolation."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[4]
s=(root/'net/ipv6/route.c').read_text()
a=s.index('static bool rt6_nh_eligible(')
b=s.index('struct fib6_info *fib6_multipath_select(',a)
c=s.index('void fib6_select_path(')
d=s.index('EXPORT_SYMBOL_GPL(fib6_select_path);',c)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define RTNH_F_DEAD 1
#define RTNH_F_LINKDOWN 16
#define RT6_LOOKUP_F_IFACE 1
#define RT6_LOOKUP_F_IGNORE_LINKSTATE 2
#define FLOWI_FLAG_SKIP_NH_OIF 4
#define RTF_GATEWAY 2
#define RTF_REJECT 512
#define RTN_BLACKHOLE 6
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define rcu_dereference(p) (p)
struct inet6_dev { struct { bool ignore_routes_with_linkdown; } cnf; } test_idev;
struct net_device { int ifindex; } dev1={1},dev2={2};
struct fib_nh_common { int unused; };
struct fib6_nh { struct fib_nh_common nh_common; struct net_device *fib_nh_dev; int fib_nh_flags, fib_nh_gw_family; };
struct nh_info { struct fib6_nh fib6_nh; bool reject_nh; } info[2];
struct nexthop { struct nh_info *nh_info; } members[2];
static int paths=2, chosen;
struct fib6_info { struct nexthop *nh; struct fib6_nh fib6_nh; int fib6_nsiblings,fib6_type,fib6_flags; } null_route;
struct net { struct { struct fib6_info *fib6_null_entry; } ipv6; } net={.ipv6={&null_route}};
struct flowi6 { int flowi6_flags,mp_hash; };
struct sk_buff { int unused; };
struct fib6_result { struct fib6_info *f6i; struct fib6_nh *nh; int fib6_type,fib6_flags; };
#define __in6_dev_get(d) (&test_idev)
static struct nexthop *nexthop_select_path(struct nexthop *nh,int hash) {return &members[chosen];}
static int nexthop_num_path(struct nexthop *nh) {return paths;}
static struct fib_nh_common *nexthop_fib_nhc(struct nexthop *nh,int i) {return &info[i].fib6_nh.nh_common;}
static int rt6_multipath_hash(const struct net *n,struct flowi6 *f,const struct sk_buff *s,void *p) {return 123;}
static struct fib6_info *fib6_multipath_select(const struct net *n,struct fib6_info *f,struct flowi6 *fl,int o,const struct sk_buff *s,int strict) {return f;}
'''+s[a:b]+s[c:d]+r'''
int main(void) {
 info[0].fib6_nh.fib_nh_dev=&dev1;
 info[1].fib6_nh.fib_nh_dev=&dev2;
 info[1].fib6_nh.fib_nh_gw_family=10;
 members[0].nh_info=&info[0];members[1].nh_info=&info[1];
 struct fib6_info route={.nh=members,.fib6_type=1};
 struct flowi6 fl={}; struct fib6_result r={.f6i=&route};
 chosen=1;fib6_select_path(&net,&r,&fl,0,NULL,0);
 assert(r.nh==&info[1].fib6_nh&&r.fib6_flags&RTF_GATEWAY);
 assert(!route.fib6_flags&&!route.fib6_nh.fib_nh_dev&&fl.mp_hash==123);
 r.f6i=&route;fib6_select_path(&net,&r,&fl,1,NULL,RT6_LOOKUP_F_IFACE);
 assert(r.nh==&info[0].fib6_nh&&!(r.fib6_flags&RTF_GATEWAY));
 info[1].fib6_nh.fib_nh_flags=RTNH_F_DEAD;
 r.f6i=&route;fib6_select_path(&net,&r,&fl,0,NULL,0);assert(r.nh==&info[0].fib6_nh);
 info[0].fib6_nh.fib_nh_flags=RTNH_F_LINKDOWN;test_idev.cnf.ignore_routes_with_linkdown=true;
 r.f6i=&route;fib6_select_path(&net,&r,&fl,0,NULL,0);assert(r.f6i==&null_route);
 r.f6i=&route;fib6_select_path(&net,&r,&fl,0,NULL,RT6_LOOKUP_F_IGNORE_LINKSTATE);assert(r.nh==&info[0].fib6_nh);
 info[0].fib6_nh.fib_nh_flags=0;info[1].fib6_nh.fib_nh_flags=0;info[1].reject_nh=true;
 r.f6i=&route;fib6_select_path(&net,&r,&fl,0,NULL,0);assert(r.fib6_type==RTN_BLACKHOLE&&r.fib6_flags&RTF_REJECT);
 fl.flowi6_flags=FLOWI_FLAG_SKIP_NH_OIF;
 r.f6i=&route;fib6_select_path(&net,&r,&fl,1,NULL,RT6_LOOKUP_F_IFACE);assert(r.nh==&info[1].fib6_nh);
 puts("PASS: production IPv6 selected member, gateway, immutable FIB, strict oif, dead/linkdown, null and blackhole results");
}
'''
with tempfile.TemporaryDirectory(prefix='nh-v6-select-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Werror',str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
