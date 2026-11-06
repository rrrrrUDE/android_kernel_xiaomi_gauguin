#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute production FIB helpers with deterministic route/neighbour stubs."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
s = (root / 'net/core/filter.c').read_text()
h = (root / 'include/uapi/linux/bpf.h').read_text()
a = h.index('/* DIRECT:  Skip the FIB rules')
b = h.index('struct bpf_redir_neigh', a)
abi = h[a:b]
a = s.index('static int bpf_fib_set_fwd_params')
b = s.index('static const struct bpf_func_proto bpf_xdp_fib_lookup_proto', a)
code = '#if 1\n' + s[a:b]
a = s.index('BPF_CALL_4(bpf_skb_fib_lookup,')
b = s.index('static const struct bpf_func_proto bpf_skb_fib_lookup_proto', a)
code += s[a:b]
c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32;
typedef uint16_t __be16; typedef uint32_t __be32;
typedef uint32_t u32;
#define __force
#define IS_ENABLED(x) 1
#define unlikely(x) (x)
#define ETH_ALEN 6
#define AF_INET 2
#define AF_INET6 10
#define IPTOS_RT_MASK 0xfc
#define RT_SCOPE_UNIVERSE 0
#define RT_TABLE_MAIN 254
#define FIB_LOOKUP_NOREF 0
#define RTN_UNICAST 1
#define RTN_BLACKHOLE 6
#define RTN_UNREACHABLE 7
#define RTN_PROHIBIT 8
#define RTF_REJECT 1
#define RTF_GATEWAY 2
#define RT6_LOOKUP_F_HAS_SADDR 1
#define NUD_VALID 1
#define IS_ERR_OR_NULL(p) (!(p))
#define BPF_CALL_4(n,t1,a1,t2,a2,t3,a3,t4,a4) static int n(t1 a1,t2 a2,t3 a3,t4 a4)
struct in6_addr { u32 words[4]; };
struct net { struct { void *fib6_null_entry; } ipv6; } test_net;
struct net_device { int ifindex,mtu; char dev_addr[6]; } test_dev={7,1500,{1,2,3,4,5,6}};
struct in_device { int forwarding; } idev4={1};
struct inet6_dev { struct { int forwarding; } cnf; } idev6={{1}};
struct neighbour { int nud_state; char ha[6]; } neigh={NUD_VALID,{6,5,4,3,2,1}};
struct fib_nh { void *fib_nh_lws; struct net_device *fib_nh_dev; int fib_nh_gw_family; union {u32 fib_nh_gw4; struct in6_addr fib_nh_gw6;}; };
struct fib_info { int fib_nhs,fib_priority; struct fib_nh fib_nh[1]; } fi={1,42,{{.fib_nh_dev=&test_dev,.fib_nh_gw_family=AF_INET,.fib_nh_gw4=99}}};
struct fib_result { int type,nh_sel; struct fib_info *fi; };
struct fib_table { int unused; } table4;
struct fib6_table { int unused; } table6;
struct fib6_info {
 int fib6_flags,fib6_type,fib6_nsiblings,fib6_metric;
 struct { void *nh_lwtstate; struct in6_addr nh_gw; struct net_device *nh_dev; } fib6_nh;
 struct { int plen; struct in6_addr addr; } fib6_prefsrc;
} f6={.fib6_type=RTN_UNICAST,.fib6_metric=43,.fib6_nh={.nh_dev=&test_dev}};
struct flowi4 { int flowi4_iif,flowi4_oif,flowi4_tos,flowi4_scope,flowi4_flags,flowi4_proto; u32 daddr,saddr,fl4_sport,fl4_dport,flowi4_multipath_hash,flowi4_mark,flowi4_secid,flowi4_uid; struct { uint64_t tun_id; } flowi4_tun_key; };
struct flowi6 { int flowi6_iif,flowi6_oif,flowi6_scope,flowi6_flags,flowi6_proto,flowi6_mark,flowi6_secid,flowi6_uid; u32 flowlabel,mp_hash,fl6_sport,fl6_dport; struct in6_addr daddr,saddr; struct { uint64_t tun_id; } flowi6_tun_key; };
static int route_err,src_err,neigh_calls,table_calls,policy_calls;
static u32 table_id,test_mtu=1400;
static bool missing_table,missing_neigh,forwardable=true;
#define dev_get_by_index_rcu(n,i) ((i)==7?&test_dev:NULL)
#define __in_dev_get_rcu(d) (&idev4)
#define IN_DEV_FORWARD(d) ((d)->forwarding)
#define __in6_dev_get_safely(d) (&idev6)
#define l3mdev_fib_table_rcu(d) 0
#define sock_net_uid(n,s) 0
#define fib_select_path(n,r,f,s) ((void)0)
#define FIB_RES_PREFSRC(n,r) 123
#define ip_mtu_from_fib_result(r,d) test_mtu
#define rt6_need_strict(a) 0
#define dev_net(d) (&test_net)
static struct fib_table *fib_get_table(struct net *n,u32 id) { table_id=id;table_calls++;return missing_table?NULL:&table4; }
static int fib_table_lookup(struct fib_table *t,struct flowi4 *f,struct fib_result *r,int flags) { *r=(struct fib_result){RTN_UNICAST,0,&fi};return route_err; }
static int fib_lookup(struct net *n,struct flowi4 *f,struct fib_result *r,int flags) {policy_calls++;return fib_table_lookup(&table4,f,r,flags);}
static struct neighbour *get_neigh(void) {neigh_calls++;return missing_neigh?NULL:&neigh;}
#define __ipv4_neigh_lookup_noref(d,a) get_neigh()
#define __ipv6_neigh_lookup_noref_stub(d,a) get_neigh()
static struct fib6_table *get_table6(struct net *n,u32 id) {table_id=id;table_calls++;return missing_table?NULL:&table6;}
static struct fib6_info *lookup6(struct net *n,struct fib6_table *t,int o,struct flowi6 *f,int strict) {return route_err?NULL:&f6;}
static struct fib6_info *policy6(struct net *n,int o,struct flowi6 *f,int strict) {policy_calls++;return lookup6(n,&table6,o,f,strict);}
static struct fib6_info *multi6(struct net *n,struct fib6_info *i,struct flowi6 *f,int o,void *s,int strict) {return i;}
static u32 mtu6(struct fib6_info *i,struct in6_addr *d,struct in6_addr *s) {return test_mtu;}
static struct {typeof(&get_table6) fib6_get_table; typeof(&lookup6) fib6_table_lookup; typeof(&policy6) fib6_lookup; typeof(&multi6) fib6_multipath_select; typeof(&mtu6) ip6_mtu_from_fib6;} stub={get_table6,lookup6,policy6,multi6,mtu6},*ipv6_stub=&stub;
static int get_src6(struct net *n,const struct net_device *d,const struct in6_addr *dst,unsigned prefs,struct in6_addr *src) {if(src_err)return src_err;src->words[0]=321;return 0;}
static struct {typeof(&get_src6) ipv6_dev_get_saddr;} bstub={get_src6},*ipv6_bpf_stub=&bstub;
struct xdp_buff {struct {struct net_device *dev;} *rxq;};
struct sk_buff {struct net_device *dev;};
#define is_skb_forwardable(d,s) forwardable
'''+abi+code+r'''
int main(void) {
 _Static_assert(sizeof(struct bpf_fib_lookup)==64,"ABI size");
 _Static_assert(__builtin_offsetof(struct bpf_fib_lookup,tbid)==48,"table ABI");
 struct {struct net_device *dev;} rx={&test_dev};
 struct xdp_buff x={.rxq=(void *)&rx};struct sk_buff skb={&test_dev};
 for(int family=AF_INET;family<=AF_INET6;family+=8) {
  struct bpf_fib_lookup p;
  for(unsigned flags=0;flags<32;flags++) {
   memset(&p,0xa5,sizeof(p));p.family=family;p.ifindex=7;p.tot_len=100;p.tbid=1234;
   neigh_calls=table_calls=policy_calls=0;
   int rc=bpf_xdp_fib_lookup(&x,&p,sizeof(p),flags);
   if(rc) fprintf(stderr,"family=%d flags=%u rc=%d\n",family,flags,rc);
   assert(rc==0);
   assert(p.ifindex==7&&p.mtu_result==1400&&p.tbid==0);
   assert(neigh_calls==!(flags&BPF_FIB_LOOKUP_SKIP_NEIGH));
   assert(table_calls==!!(flags&BPF_FIB_LOOKUP_DIRECT));
   assert(policy_calls==!(flags&BPF_FIB_LOOKUP_DIRECT));
   if(flags&BPF_FIB_LOOKUP_DIRECT) assert(table_id==((flags&BPF_FIB_LOOKUP_TBID)?1234:254));
   if(flags&BPF_FIB_LOOKUP_SKIP_NEIGH) assert(p.smac[0]==0xa5&&p.dmac[0]==0xa5);
   else assert(p.smac[0]==1&&p.dmac[0]==6);
   if(flags&BPF_FIB_LOOKUP_SRC) assert((family==AF_INET?p.ipv4_src:p.ipv6_src[0])==(family==AF_INET?123:321));
  }
  p=(struct bpf_fib_lookup){.family=family,.ifindex=7,.tot_len=1500};
  assert(bpf_xdp_fib_lookup(&x,&p,sizeof(p),0)==BPF_FIB_LKUP_RET_FRAG_NEEDED&&p.mtu_result==1400);
  p.tot_len=100;missing_neigh=true;
  assert(bpf_xdp_fib_lookup(&x,&p,sizeof(p),0)==BPF_FIB_LKUP_RET_NO_NEIGH);
  assert(bpf_xdp_fib_lookup(&x,&p,sizeof(p),BPF_FIB_LOOKUP_SKIP_NEIGH)==0);missing_neigh=false;
  p.tot_len=100;missing_table=true;p.tbid=1234;
  assert(bpf_xdp_fib_lookup(&x,&p,sizeof(p),BPF_FIB_LOOKUP_DIRECT|BPF_FIB_LOOKUP_TBID)==BPF_FIB_LKUP_RET_NOT_FWDED&&p.tbid==0);
  missing_table=false;
  if(family==AF_INET) {
   int errors[]={-EINVAL,-EHOSTUNREACH,-EACCES,-ENOENT};
   int expected[]={BPF_FIB_LKUP_RET_BLACKHOLE,BPF_FIB_LKUP_RET_UNREACHABLE,BPF_FIB_LKUP_RET_PROHIBIT,BPF_FIB_LKUP_RET_NOT_FWDED};
   for(int i=0;i<4;i++){route_err=errors[i];assert(bpf_xdp_fib_lookup(&x,&p,sizeof(p),0)==expected[i]);}
   route_err=0;idev4.forwarding=0;
   assert(bpf_xdp_fib_lookup(&x,&p,sizeof(p),0)==BPF_FIB_LKUP_RET_FWD_DISABLED);idev4.forwarding=1;
  } else {
   f6.fib6_flags=RTF_REJECT;f6.fib6_type=RTN_BLACKHOLE;
   assert(bpf_xdp_fib_lookup(&x,&p,sizeof(p),0)==BPF_FIB_LKUP_RET_BLACKHOLE);
   f6.fib6_flags=0;f6.fib6_type=RTN_UNICAST;
  }
  p.tot_len=0;forwardable=false;
  assert(bpf_skb_fib_lookup(&skb,&p,sizeof(p),0)==BPF_FIB_LKUP_RET_FRAG_NEEDED&&p.mtu_result==1500);forwardable=true;
  p.tot_len=0;assert(bpf_skb_fib_lookup(&skb,&p,sizeof(p),0)==0&&p.mtu_result==1500);
 }
 /* IPv4 destination routed through an IPv6 neighbour. */
 struct bpf_fib_lookup via={.family=AF_INET,.ifindex=7,.tot_len=100};
 fi.fib_nh[0].fib_nh_gw_family=AF_INET6;
 fi.fib_nh[0].fib_nh_gw6=(struct in6_addr){{11,22,33,44}};
 assert(bpf_xdp_fib_lookup(&x,&via,sizeof(via),BPF_FIB_LOOKUP_SRC)==0);
 assert(via.family==AF_INET6&&via.ipv6_dst[0]==11&&via.ipv6_dst[3]==44&&via.ipv4_src==123);
 via=(struct bpf_fib_lookup){.family=AF_INET,.ifindex=7,.tot_len=100};
 neigh_calls=0;
 assert(bpf_xdp_fib_lookup(&x,&via,sizeof(via),BPF_FIB_LOOKUP_SKIP_NEIGH)==0&&!neigh_calls&&via.family==AF_INET6);
 fi.fib_nh[0].fib_nh_gw_family=AF_INET;
 fi.fib_nh[0].fib_nh_gw4=99;
 struct bpf_fib_lookup p={.family=AF_INET6,.ifindex=7,.tot_len=100};
 src_err=-EADDRNOTAVAIL;neigh_calls=0;
 assert(bpf_xdp_fib_lookup(&x,&p,sizeof(p),BPF_FIB_LOOKUP_SRC)==BPF_FIB_LKUP_RET_NO_SRC_ADDR&&!neigh_calls);
 f6.fib6_prefsrc.plen=128;f6.fib6_prefsrc.addr.words[0]=456;
 assert(bpf_xdp_fib_lookup(&x,&p,sizeof(p),BPF_FIB_LOOKUP_SRC)==0&&p.ipv6_src[0]==456);
 assert(bpf_xdp_fib_lookup(&x,&p,63,0)==-EINVAL);
 assert(bpf_xdp_fib_lookup(&x,&p,64,32)==-EINVAL);
 assert(bpf_skb_fib_lookup(&skb,&p,63,0)==-EINVAL);
 assert(bpf_skb_fib_lookup(&skb,&p,64,32)==-EINVAL);
 puts("PASS: production IPv4/IPv6 FIB flags, table/policy lookup, neighbour bypass, source selection, MTU and ABI");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.c';p.write_text(c)
    subprocess.run(['cc',str(p),'-o',d+'/test'],check=True)
    subprocess.run([d+'/test'],check=True)
