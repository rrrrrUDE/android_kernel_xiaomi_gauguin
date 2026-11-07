#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise production IPv4 nexthop init/release with allocation fault injection.

Host stubs model ownership, not routing or RCU concurrency. This does not test
RTM_NEWNEXTHOP or groups, which are not implemented by these prerequisites.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
src = (root / 'net/ipv4/fib_semantics.c').read_text()

def function(name):
    start = src.index(name)
    brace = src.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
typedef unsigned short u16;
typedef unsigned int gfp_t;
#define AF_INET 2
#define AF_INET6 10
#define GFP_KERNEL 0
#define LWTUNNEL_ENCAP_NONE 0
#define NL_SET_ERR_MSG(e,m) ((void)0)
struct in6_addr { unsigned char bytes[16]; };
struct net { struct { int fib_num_tclassid_users; } ipv4; };
struct netlink_ext_ack { int unused; };
struct nlattr { int unused; };
struct rtable { int unused; };
struct lwtunnel_state { int unused; };
struct net_device { int refs; };
struct fib_nh_common {
 int nhc_family;
 struct rtable **nhc_pcpu_rth_output;
 struct rtable *nhc_rth_input;
 void *nhc_exceptions;
 struct lwtunnel_state *nhc_lwtstate;
};
struct fib_nh {
 struct fib_nh_common nh_common;
 int fib_nh_oif, fib_nh_gw_family, fib_nh_flags, fib_nh_weight;
 union { uint32_t fib_nh_gw4; struct in6_addr fib_nh_gw6; };
 unsigned nh_tclassid;
 struct net_device *fib_nh_dev;
};
#define fib_nh_family nh_common.nhc_family
struct fib_config {
 struct nlattr *fc_encap;
 u16 fc_encap_type;
 int fc_oif, fc_gw_family, fc_flags;
 uint32_t fc_gw;
 struct in6_addr fc_gw6;
 unsigned fc_flow;
};
static int alloc_fail, lwt_fail, allocations, lwts, exception_frees, route_frees;
static void *pcpu_alloc(void) {
 if (alloc_fail) return NULL;
 ++allocations;
 return calloc(1, sizeof(void *));
}
#define alloc_percpu_gfp(t,g) pcpu_alloc()
static void rt_fibinfo_free_cpus(void *p) { if(p) { --allocations; free(p); } }
static void rt_fibinfo_free(struct rtable **p) { if(*p) { ++route_frees; free(*p); } }
static void free_nh_exceptions(struct fib_nh_common *p) {
 if(p->nhc_exceptions) { ++exception_frees; free(p->nhc_exceptions); }
}
static int lwtunnel_build_state(u16 t, struct nlattr *e, int f, void *cfg,
 struct lwtunnel_state **out, struct netlink_ext_ack *ack) {
 if(lwt_fail) return -EIO;
 *out=calloc(1,sizeof(**out)); ++lwts; return 0;
}
#define lwtstate_get(p) (p)
static void lwtstate_put(void *p) { if(p) { --lwts; free(p); } }
#define RCU_INIT_POINTER(p,v) ((p)=(v))
#define atomic_inc(p) (++*(p))
#define atomic_dec(p) (--*(p))
static void dev_put(struct net_device *d) { assert(d->refs>0); --d->refs; }
'''
for name in ('int fib_nh_common_init(', 'int fib_nh_init(',
             'void fib_nh_common_release(', 'void fib_nh_release('):
    code += function(name) + '\n'
code += r'''
int main(void) {
 struct net net={}; struct fib_nh nh={}; struct nlattr encap={};
 struct net_device dev={.refs=1};
 struct fib_config cfg={.fc_oif=7,.fc_gw=0x01020304,.fc_flow=99};
 alloc_fail=1;
 assert(fib_nh_init(&net,&nh,&cfg,3,NULL)==-ENOMEM);
 assert(!allocations && !net.ipv4.fib_num_tclassid_users);
 fib_nh_release(&net,&nh);
 alloc_fail=0; cfg.fc_encap=&encap;
 assert(fib_nh_init(&net,&nh,&cfg,3,NULL)==-EINVAL);
 assert(!allocations && !lwts && !net.ipv4.fib_num_tclassid_users);
 fib_nh_release(&net,&nh);
 cfg.fc_encap_type=1; lwt_fail=1;
 assert(fib_nh_init(&net,&nh,&cfg,3,NULL)==-EIO);
 assert(!allocations && !lwts);
 fib_nh_release(&net,&nh);
 lwt_fail=0;
 assert(!fib_nh_init(&net,&nh,&cfg,3,NULL));
 assert(allocations==1 && lwts==1 && nh.fib_nh_gw_family==AF_INET);
 assert(nh.fib_nh_gw4==cfg.fc_gw && nh.fib_nh_oif==7);
#ifdef CONFIG_IP_ROUTE_CLASSID
 assert(net.ipv4.fib_num_tclassid_users==1);
#endif
#ifdef CONFIG_IP_ROUTE_MULTIPATH
 assert(nh.fib_nh_weight==3);
#endif
 nh.fib_nh_dev=&dev;
 nh.nh_common.nhc_exceptions=malloc(1);
 nh.nh_common.nhc_rth_input=calloc(1,sizeof(struct rtable));
 fib_nh_release(&net,&nh);
 assert(!dev.refs && !allocations && !lwts && !net.ipv4.fib_num_tclassid_users);
 assert(exception_frees==1 && route_frees==1);
 cfg.fc_gw_family=AF_INET6; cfg.fc_gw6.bytes[15]=1;
 assert(!fib_nh_init(&net,&nh,&cfg,1,NULL));
 assert(nh.fib_nh_gw_family==AF_INET6 && nh.fib_nh_gw6.bytes[15]==1);
 fib_nh_release(&net,&nh);
 assert(!allocations && !lwts && !net.ipv4.fib_num_tclassid_users);
 puts("PASS: production nexthop init/release, IPv4/IPv6 gateways, allocation/LWT failures and resource accounting");
}
'''
with tempfile.TemporaryDirectory(prefix='nh-init-') as temp:
    source = Path(temp) / 'test.c'
    source.write_text(code)
    for flags in ([], ['-DCONFIG_IP_ROUTE_CLASSID', '-DCONFIG_IP_ROUTE_MULTIPATH']):
        binary = Path(temp) / 'test'
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', *flags,
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
