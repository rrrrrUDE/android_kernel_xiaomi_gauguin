#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check production common nexthop netlink serialization with host stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
src = (root / 'net/ipv4/fib_semantics.c').read_text()
a = src.index('int fib_nexthop_info(')
b = src.index('int fib_nh_put_gateway(', a)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define IS_ENABLED(x) (x)
#define CONFIG_IP_ROUTE_MULTIPATH 1
#define EXPORT_SYMBOL_GPL(x)
#define AF_INET 2
#define AF_INET6 10
#define RTNH_F_DEAD 1
#define RTNH_F_ONLINK 4
#define RTNH_F_OFFLOAD 8
#define RTNH_F_LINKDOWN 16
#define RTNH_F_TRAP 64
#define RTA_OIF 4
#define RTA_GATEWAY 5
#define RTA_FLOW 11
#define RTA_VIA 18
#define RTA_ENCAP_TYPE 21
#define RTA_ENCAP 22
struct in_device { bool ignore; };
struct inet6_dev { struct { bool ignore_routes_with_linkdown; } cnf; };
struct net_device { int ifindex; struct in_device in; struct inet6_dev in6; };
struct in6_addr { u8 bytes[16]; };
struct fib_nh_common {
 unsigned nhc_flags;
 int nhc_family, nhc_gw_family;
 struct net_device *nhc_dev;
 union { u32 ipv4; struct in6_addr ipv6; } nhc_gw;
 void *nhc_lwtstate;
};
struct nlattr { uint16_t len, type; };
struct rtvia { uint16_t rtvia_family; u8 rtvia_addr[]; };
struct rtnexthop { uint16_t rtnh_len; u8 rtnh_flags, rtnh_hops; int rtnh_ifindex; };
struct sk_buff { unsigned len, limit; u8 data[512]; };
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define __in_dev_get_rcu(d) (&(d)->in)
#define __in6_dev_get(d) (&(d)->in6)
#define IN_DEV_IGNORE_ROUTES_WITH_LINKDOWN(d) ((d)->ignore)
static void *nla_data(struct nlattr *a) { return a+1; }
static void *nlmsg_get_pos(struct sk_buff *s) { return s->data+s->len; }
static void *nla_reserve_nohdr(struct sk_buff *s, unsigned n) {
 if(s->len+n>s->limit) return NULL;
 void *p=s->data+s->len; memset(p,0,n); s->len+=n; return p;
}
static struct nlattr *nla_reserve(struct sk_buff *s,int type,unsigned size) {
 unsigned n=(sizeof(struct nlattr)+size+3)&~3U;
 struct nlattr *a=nla_reserve_nohdr(s,n);
 if(a) { a->len=sizeof(*a)+size; a->type=type; } return a;
}
static int nla_put(struct sk_buff *s,int type,unsigned size,const void *p) {
 struct nlattr *a=nla_reserve(s,type,size);
 if(!a) return -EMSGSIZE;
 memcpy(nla_data(a),p,size); return 0;
}
static int nla_put_u32(struct sk_buff *s,int type,u32 val) {return nla_put(s,type,4,&val);}
#define nla_put_in_addr nla_put_u32
static int nla_put_in6_addr(struct sk_buff *s,int type,const struct in6_addr *a) {
 return nla_put(s,type,sizeof(*a),a);
}
static int lwtunnel_fill_encap(struct sk_buff *s,void *p,int encap,int type) {
 assert(encap==RTA_ENCAP && type==RTA_ENCAP_TYPE);
 return nla_put_u32(s,encap,0x1234);
}
'''+src[a:b]+r'''
static struct nlattr *find(struct sk_buff *s,unsigned start,int type) {
 for(unsigned i=start;i<s->len;) {
  struct nlattr *a=(void *)(s->data+i);
  assert(a->len>=sizeof(*a) && i+a->len<=s->len);
  if(a->type==type) return a;
  i+=(a->len+3)&~3U;
 }
 return NULL;
}
int main(void) {
 struct net_device dev={.ifindex=7};
 struct fib_nh_common nh={.nhc_dev=&dev,.nhc_family=AF_INET,.nhc_gw_family=AF_INET};
 struct sk_buff s={.limit=sizeof(s.data)}; u8 flags=0;
 nh.nhc_gw.ipv4=0x04030201;
 nh.nhc_flags=RTNH_F_ONLINK|RTNH_F_OFFLOAD|RTNH_F_TRAP|RTNH_F_LINKDOWN;
 dev.in.ignore=true;
 assert(!fib_nexthop_info(&s,&nh,AF_INET,&flags,false));
 assert(flags==(nh.nhc_flags|RTNH_F_DEAD));
 assert(find(&s,0,RTA_GATEWAY) && find(&s,0,RTA_OIF));
 assert(!find(&s,0,RTA_VIA));
 s.len=0; nh.nhc_gw_family=AF_INET6; nh.nhc_gw.ipv6.bytes[15]=1;
 assert(!fib_nexthop_info(&s,&nh,AF_INET,&flags,true));
 struct nlattr *a=find(&s,0,RTA_VIA); assert(a && a->len==22);
 struct rtvia *v=nla_data(a); assert(v->rtvia_family==AF_INET6 && v->rtvia_addr[15]==1);
 assert(!find(&s,0,RTA_GATEWAY) && !find(&s,0,RTA_OIF));
 s.len=0; nh.nhc_family=AF_INET6;
 assert(!fib_nexthop_info(&s,&nh,AF_INET6,&flags,false));
 assert(find(&s,0,RTA_GATEWAY) && !find(&s,0,RTA_VIA));
 nh.nhc_lwtstate=&nh; s.len=0;
 assert(!fib_add_nexthop(&s,&nh,256,AF_INET6,42));
 struct rtnexthop *r=(void *)s.data;
 assert(r->rtnh_hops==255 && r->rtnh_ifindex==7 && r->rtnh_len==s.len);
 assert(find(&s,sizeof(*r),RTA_FLOW) && find(&s,sizeof(*r),RTA_ENCAP));
 /* Every truncation of a valid message must report insufficient space. */
 unsigned len=s.len;
 for(unsigned lim=0;lim<len;lim++) {
  s.len=0; s.limit=lim;
  assert(fib_add_nexthop(&s,&nh,256,AF_INET6,42)==-EMSGSIZE);
 }
 s.len=0; s.limit=sizeof(s.data); nh=(struct fib_nh_common){};
 assert(!fib_add_nexthop(&s,&nh,1,AF_INET,0));
 r=(void *)s.data; assert(r->rtnh_ifindex==0 && r->rtnh_hops==0 && r->rtnh_len==sizeof(*r));
 puts("PASS: production nexthop gateway families, flags, weights, LWT, classid and netlink space failures");
}
'''
with tempfile.TemporaryDirectory(prefix='nh-dump-') as temp:
    source = Path(temp) / 'test.c'
    source.write_text(code)
    for ipv6 in (0, 1):
        binary = Path(temp) / 'test'
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror',
                        f'-DCONFIG_IPV6={ipv6}', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
