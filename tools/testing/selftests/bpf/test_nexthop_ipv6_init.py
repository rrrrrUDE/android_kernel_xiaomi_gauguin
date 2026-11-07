#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Fault-inject production IPv6 nexthop initialization with ownership stubs."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
src = (root / 'net/ipv6/route.c').read_text()
a = src.index('int fib6_nh_init(')
b = src.index('\nEXPORT_SYMBOL_GPL(fib6_nh_init)', a)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef unsigned gfp_t;
#define AF_INET6 10
#define RTNH_F_ONLINK 4
#define RTNH_F_LINKDOWN 16
#define RTF_REJECT 0x200
#define RTF_GATEWAY 2
#define RTF_LOCAL 0x80000000U
#define RTF_ANYCAST 0x100000
#define IFF_UP 1
#define NL_SET_ERR_MSG(e,m) ((void)0)
struct in6_addr { unsigned char bytes[16]; };
struct net_device { int refs, ifindex, flags; bool carrier; };
struct inet6_dev { int refs; struct { bool disable_ipv6; } cnf; };
struct net { struct net_device *loopback_dev; };
struct netlink_ext_ack { int unused; };
struct fib6_config {
 bool fc_is_fdb; int fc_ifindex; unsigned fc_flags;
 struct in6_addr fc_gateway; void *fc_encap; unsigned short fc_encap_type;
};
struct rt6_info { int unused; };
struct fib_nh_common { void *nhc_pcpu_rth_output, *nhc_lwtstate; };
struct fib6_nh {
 struct fib_nh_common nh_common;
 int fib_nh_family, fib_nh_gw_family, fib_nh_flags, fib_nh_weight;
 struct in6_addr fib_nh_gw6;
 struct net_device *fib_nh_dev; int fib_nh_oif;
 void *rt6i_pcpu;
};
#define fib_nh_lws nh_common.nhc_lwtstate
static struct net_device device, loopback;
static struct inet6_dev idev;
static int missing_idev, common_fail, pcpu_fail, gw_fail, common_refs;
static struct net_device *dev_get_by_index(struct net *n,int i) {
 if(i!=7) return NULL; ++device.refs; return &device;
}
static struct inet6_dev *in6_dev_get(struct net_device *d) {
 if(missing_idev) return NULL; ++idev.refs; return &idev;
}
static void dev_hold(struct net_device *d) { ++d->refs; }
static void dev_put(struct net_device *d) { assert(d->refs>0); --d->refs; }
static void in6_dev_put(struct inet6_dev *d) { assert(d->refs>0); --d->refs; }
#define netif_carrier_ok(d) ((d)->carrier)
static int ip6_validate_gw(struct net *n,struct fib6_config *c,
 struct net_device **d,struct inet6_dev **i,struct netlink_ext_ack *a) { return gw_fail; }
static int fib_nh_common_init(struct net *n,struct fib_nh_common *h,void *encap,
 unsigned short type,void *cfg,gfp_t g,struct netlink_ext_ack *a) {
 if(common_fail) return -ENOMEM;
 h->nhc_pcpu_rth_output=malloc(1); ++common_refs; return 0;
}
static void fib_nh_common_release(struct fib_nh_common *h) {
 if(h->nhc_pcpu_rth_output) { free(h->nhc_pcpu_rth_output); --common_refs; }
 h->nhc_pcpu_rth_output=NULL;
}
static void *pcpu_alloc(void) { return pcpu_fail ? NULL : calloc(1,sizeof(void *)); }
#define alloc_percpu_gfp(t,g) pcpu_alloc()
'''+src[a:b]+r'''
static void cleanup(struct fib6_nh *nh) {
 if(nh->fib_nh_dev) dev_put(nh->fib_nh_dev);
 free(nh->rt6i_pcpu); fib_nh_common_release(&nh->nh_common);
 memset(nh,0,sizeof(*nh));
 assert(!device.refs && !loopback.refs && !idev.refs && !common_refs);
}
int main(void) {
 struct net net={.loopback_dev=&loopback}; struct fib6_nh nh={};
 struct fib6_config cfg={.fc_ifindex=7};
 device.ifindex=7; device.flags=IFF_UP; device.carrier=true;
 loopback.ifindex=1; loopback.flags=IFF_UP; loopback.carrier=true;
 cfg.fc_is_fdb=true; cfg.fc_gateway.bytes[15]=9;
 assert(!fib6_nh_init(&net,&nh,&cfg,0,NULL));
 assert(nh.fib_nh_gw_family==AF_INET6 && nh.fib_nh_gw6.bytes[15]==9);
 assert(!nh.fib_nh_dev && !nh.rt6i_pcpu && !common_refs); cleanup(&nh);
 cfg.fc_is_fdb=false; cfg.fc_ifindex=99;
 assert(fib6_nh_init(&net,&nh,&cfg,0,NULL)==-ENODEV); cleanup(&nh);
 cfg.fc_ifindex=7; missing_idev=1;
 assert(fib6_nh_init(&net,&nh,&cfg,0,NULL)==-ENODEV); cleanup(&nh);
 missing_idev=0; cfg.fc_flags=RTNH_F_ONLINK; device.flags=0;
 assert(fib6_nh_init(&net,&nh,&cfg,0,NULL)==-ENETDOWN); cleanup(&nh);
 device.flags=IFF_UP; cfg.fc_flags=0; idev.cnf.disable_ipv6=true;
 assert(fib6_nh_init(&net,&nh,&cfg,0,NULL)==-EACCES); cleanup(&nh);
 idev.cnf.disable_ipv6=false; common_fail=1;
 assert(fib6_nh_init(&net,&nh,&cfg,0,NULL)==-ENOMEM); cleanup(&nh);
 common_fail=0; pcpu_fail=1;
 assert(fib6_nh_init(&net,&nh,&cfg,0,NULL)==-ENOMEM); cleanup(&nh);
 pcpu_fail=0; cfg.fc_flags=RTF_GATEWAY; gw_fail=-EINVAL;
 assert(fib6_nh_init(&net,&nh,&cfg,0,NULL)==-EINVAL); cleanup(&nh);
 gw_fail=0; device.carrier=false;
 assert(!fib6_nh_init(&net,&nh,&cfg,0,NULL));
 assert(nh.fib_nh_dev==&device && nh.fib_nh_oif==7);
 assert(nh.fib_nh_flags & RTNH_F_LINKDOWN);
 assert(nh.fib_nh_gw6.bytes[15]==9); cleanup(&nh);
 cfg.fc_flags=RTF_REJECT;
 assert(!fib6_nh_init(&net,&nh,&cfg,0,NULL));
 assert(nh.fib_nh_dev==&loopback); cleanup(&nh);
 /* Native routes arrive with an empty per-CPU array already allocated. */
 cfg.fc_flags=RTF_LOCAL; nh.rt6i_pcpu=pcpu_alloc(); void *pcpu=nh.rt6i_pcpu;
 assert(!fib6_nh_init(&net,&nh,&cfg,0,NULL));
 assert(nh.rt6i_pcpu==pcpu && !(nh.fib_nh_flags & RTNH_F_LINKDOWN)); cleanup(&nh);
 puts("PASS: production IPv6 nexthop FDB, gateway/device validation, reject route and allocation failure ownership");
}
'''
with tempfile.TemporaryDirectory(prefix='nh-v6-init-') as temp:
    source=Path(temp)/'test.c'; binary=Path(temp)/'test'
    source.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Werror','-Wno-misleading-indentation',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
