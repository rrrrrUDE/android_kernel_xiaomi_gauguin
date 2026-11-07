#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run production gateway parsing, matching and serialization with host stubs."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
s = (root/'net/ipv4/fib_frontend.c').read_text()
a=s.index('int fib_gw_from_via('); b=s.index('static int rtm_to_fib_config',a)
parse=s[a:b]
s=(root/'net/ipv4/fib_semantics.c').read_text()
a=s.index('int fib_nh_put_gateway(');b=s.index('int fib_dump_info(',a)
put=s[a:b]
s=(root/'include/net/ip_fib.h').read_text()
a=s.index('static inline bool fib_nh_gw_match(');b=s.index('\n}',a)+2
match=s[a:b]
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t __be32;
typedef uint16_t __kernel_sa_family_t;
#define __packed __attribute__((packed))
#define AF_INET 2
#define AF_INET6 10
#define RTA_GATEWAY 5
#define RTA_VIA 18
#define NL_SET_ERR_MSG(e,m) ((void)0)
#define IS_ENABLED(x) IPV6_ENABLED
struct in6_addr {uint8_t bytes[16];};
struct rtvia {uint16_t rtvia_family; uint8_t rtvia_addr[];};
struct nlattr {int len; uint8_t data[32];};
#define nla_len(n) ((n)->len)
#define nla_data(n) ((void *)(n)->data)
struct netlink_ext_ack {int unused;};
struct fib_config {u8 fc_gw_family;__be32 fc_gw;struct in6_addr fc_gw6;};
struct fib_nh {u8 fib_nh_gw_family;union {__be32 fib_nh_gw4;struct in6_addr fib_nh_gw6;};};
struct sk_buff {int type,len;uint8_t data[32];};
static int put_error;
static int nla_put(struct sk_buff *s,int t,int l,const void *d) {if(put_error)return -EMSGSIZE;s->type=t;s->len=l;memcpy(s->data,d,l);return 0;}
static int nla_put_in_addr(struct sk_buff *s,int t,__be32 a) {return nla_put(s,t,sizeof(a),&a);}
'''+parse+match+put+r'''
int main(void) {
 struct nlattr n={};struct fib_config c={};struct fib_nh nh={};struct sk_buff s={};
 struct rtvia *v=(void *)n.data;
 n.len=1;assert(fib_gw_from_via(&c,&n,NULL)==-EINVAL);
 v->rtvia_family=AF_INET;n.len=5;assert(fib_gw_from_via(&c,&n,NULL)==-EINVAL);
 n.len=6;uint32_t gw=0x04030201;memcpy(v->rtvia_addr,&gw,4);
 assert(!fib_gw_from_via(&c,&n,NULL)&&c.fc_gw_family==AF_INET&&c.fc_gw==gw);
 nh.fib_nh_gw_family=AF_INET;nh.fib_nh_gw4=gw;
 assert(fib_nh_gw_match(&c,&nh));nh.fib_nh_gw4++;assert(!fib_nh_gw_match(&c,&nh));nh.fib_nh_gw4=gw;
 assert(!fib_nh_put_gateway(&s,&nh)&&s.type==RTA_GATEWAY&&s.len==4&&!memcmp(s.data,&gw,4));
 c=(struct fib_config){};assert(fib_nh_gw_match(&c,&nh));
 c.fc_gw=gw;assert(fib_nh_gw_match(&c,&nh)); /* ioctl legacy family */
 n.len=18;v->rtvia_family=AF_INET6;for(int i=0;i<16;i++)v->rtvia_addr[i]=i+1;
#if IPV6_ENABLED
 assert(!fib_gw_from_via(&c,&n,NULL)&&c.fc_gw_family==AF_INET6&&!memcmp(c.fc_gw6.bytes,v->rtvia_addr,16));
 nh.fib_nh_gw_family=AF_INET6;nh.fib_nh_gw6=c.fc_gw6;
 assert(fib_nh_gw_match(&c,&nh));nh.fib_nh_gw6.bytes[15]++;assert(!fib_nh_gw_match(&c,&nh));nh.fib_nh_gw6=c.fc_gw6;
 assert(!fib_nh_put_gateway(&s,&nh)&&s.type==RTA_VIA&&s.len==18&&!memcmp(s.data,n.data,18));
 n.len=17;assert(fib_gw_from_via(&c,&n,NULL)==-EINVAL);
 n.len=19;assert(fib_gw_from_via(&c,&n,NULL)==-EINVAL);
#else
 assert(fib_gw_from_via(&c,&n,NULL)==-EINVAL);
#endif
 v->rtvia_family=0;n.len=18;assert(fib_gw_from_via(&c,&n,NULL)==-EINVAL);
 put_error=1;assert(fib_nh_put_gateway(&s,&nh)==-EMSGSIZE);
 nh.fib_nh_gw_family=0;s.len=0;assert(!fib_nh_put_gateway(&s,&nh)&&!s.len);
 puts("PASS: production RTA_VIA parse/match/dump, malformed lengths, families and serialization errors");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.c';p.write_text(c)
    for enabled in (0,1):
        subprocess.run(['cc','-DIPV6_ENABLED='+str(enabled),str(p),'-o',d+'/test'],check=True)
        subprocess.run([d+'/test'],check=True)
