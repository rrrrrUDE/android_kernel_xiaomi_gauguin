#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise production FIB RCU destruction and shared-object comparisons.

Stubs check ownership and metrics; they do not model concurrent RCU readers.
"""
from pathlib import Path
import subprocess
import tempfile

src = (Path(__file__).resolve().parents[4] / 'net/ipv4/fib_semantics.c').read_text()
def function(signature):
    start = src.index(signature)
    brace = src.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
typedef unsigned char u8;
struct netlink_ext_ack {int unused;};
#define NL_SET_ERR_MSG(e,m) ((void)0)
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define AF_INET6 10
#define RTNH_COMPARE_MASK 0
struct rcu_head {int unused;};
struct nexthop {int refs; int scope_error;};
struct dst_metrics {int refcnt;};
static struct dst_metrics dst_default_metrics;
struct fib_nh {
 int fib_nh_oif, fib_nh_gw_family, fib_nh_gw6, fib_nh_gw4;
 int fib_nh_scope, fib_nh_lws, fib_nh_flags;
};
struct fib_info {
 struct nexthop *nh;
 struct rcu_head rcu;
 struct dst_metrics *fib_metrics;
 int fib_net, fib_nhs;
 struct fib_nh fib_nh[2];
};
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define change_nexthops(fi) for(int i=0;i<(fi)->fib_nhs;i++) {struct fib_nh *nexthop_nh=&(fi)->fib_nh[i];
#define for_nexthops(fi) for(int i=0;i<(fi)->fib_nhs;i++) {const struct fib_nh *nh=&(fi)->fib_nh[i];
#define endfor_nexthops(fi) }
static int releases, frees;
static int fib_check_nexthop(struct nexthop *nh,u8 scope,struct netlink_ext_ack *e) {return nh->scope_error;}
static bool nexthop_get(struct nexthop *nh) {if(!nh->refs)return false;++nh->refs;return true;}
static void nexthop_put(struct nexthop *nh) {assert(nh->refs>0);--nh->refs;}
static void fib_nh_release(int net,struct fib_nh *nh) {++releases;}
static int refcount_dec_and_test(int *p) {assert(*p>0);return --*p==0;}
static void kfree(void *p) {++frees;free(p);}
static int lwtunnel_cmp_encap(int a,int b) {return a!=b;}
'''
code += function('static void free_fib_info_rcu(') + '\n'
code += function('static inline int nh_comp(') + '\n'
code += function('static int fib_info_attach_nh(') + '\n'
code += r'''
int main(void) {
 struct fib_info attach={};struct nexthop target={.refs=1,.scope_error=-EINVAL};
 assert(fib_info_attach_nh(&attach,&target,0,NULL)==-EINVAL);
 assert(!attach.nh && target.refs==1);
 target.scope_error=0;target.refs=0;
 assert(fib_info_attach_nh(&attach,&target,0,NULL)==-EINVAL);
 assert(!attach.nh && target.refs==0);
 target.refs=1;
 assert(fib_info_attach_nh(&attach,&target,0,NULL)==0);
 assert(attach.nh==&target && target.refs==2);
 nexthop_put(attach.nh);assert(target.refs==1);
 struct nexthop a={.refs=2},b={.refs=1};
 struct fib_info x={.nh=&a},y={.nh=&a};
 assert(nh_comp(&x,&y)==0);
 y.nh=&b;assert(nh_comp(&x,&y)!=0);
 y.nh=NULL;assert(nh_comp(&x,&y)!=0);
 x.nh=NULL;x.fib_nhs=y.fib_nhs=1;
 assert(nh_comp(&x,&y)==0);
 y.fib_nh[0].fib_nh_oif=3;assert(nh_comp(&x,&y)!=0);
 struct fib_info *fi=calloc(1,sizeof(*fi));
 fi->nh=&a;fi->fib_metrics=&dst_default_metrics;
 free_fib_info_rcu(&fi->rcu);
 assert(a.refs==1 && releases==0 && frees==1);
 fi=calloc(1,sizeof(*fi));fi->fib_nhs=2;
 fi->fib_metrics=calloc(1,sizeof(*fi->fib_metrics));
 fi->fib_metrics->refcnt=1;
 free_fib_info_rcu(&fi->rcu);
 assert(releases==2 && frees==3);
 fi=calloc(1,sizeof(*fi));fi->nh=&a;
 struct dst_metrics *metrics=calloc(1,sizeof(*metrics));
 metrics->refcnt=2;fi->fib_metrics=metrics;
 free_fib_info_rcu(&fi->rcu);
 assert(a.refs==0 && metrics->refcnt==1 && releases==2 && frees==4);
 free(metrics);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nh-lifetime-') as tmp:
    source, binary = Path(tmp)/'test.c', Path(tmp)/'test'
    source.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Werror',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: production FIB shared/embedded RCU ownership, metrics and object identity')
