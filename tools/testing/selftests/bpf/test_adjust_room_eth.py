#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise production encapsulation bookkeeping; packet allocation is stubbed."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[4]
s=(root/'net/core/filter.c').read_text()
h=(root/'include/uapi/linux/bpf.h').read_text()
a=h.index('\tBPF_F_ADJ_ROOM_FIXED_GSO'); b=h.index('\n};',a)
flags='enum {\n'+h[a:b]+'\n};\n'
a=s.index('#define BPF_F_ADJ_ROOM_ENCAP_L3_MASK');b=s.index('static int bpf_skb_net_shrink',a)
c=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define BPF_ADJ_ROOM_ENCAP_L2_SHIFT 56
#define BPF_ADJ_ROOM_ENCAP_L2_MASK 255
#define BPF_F_ADJ_ROOM_ENCAP_L2(n) ((u64)(n)<<56)
#define ENOTSUPP 524
#define ETH_HLEN 14
#define ETH_P_IP 0x800
#define ETH_P_IPV6 0x86dd
#define ETH_P_TEB 0x6558
#define htons(x) (x)
#define unlikely(x) (x)
#define SKB_GSO_DODGY 1
#define SKB_GSO_UDP_L4 2
#define SKB_GSO_UDP_TUNNEL 4
#define SKB_GSO_GRE 8
#define SKB_GSO_IPXIP6 16
#define SKB_GSO_IPXIP4 32
struct iphdr { char bytes[20]; }; struct ipv6hdr { char bytes[40]; };
struct skb_shared_info { int gso_type, gso_segs; void *frag_list; };
struct sk_buff { u16 protocol,inner_protocol; int network_header,mac_header,transport_header,inner_mac_header,inner_network_header,inner_transport_header; bool encapsulation; struct skb_shared_info shinfo; };
#define skb_is_gso(s) 0
#define skb_is_gso_tcp(s) 0
#define skb_shinfo(s) (&(s)->shinfo)
#define skb_cow_head(s,n) 0
#define bpf_skb_net_hdr_push(s,o,n) 0
#define skb_set_inner_protocol(s,p) ((s)->inner_protocol=(p))
#define skb_set_network_header(s,n) ((s)->network_header=(n))
#define skb_set_transport_header(s,n) ((s)->transport_header=(n))
#define skb_decrease_gso_size(s,n) ((void)0)
#define skb_linearize(s) 0
'''+flags+s[a:b]+r'''
int main(void) {
 struct sk_buff s;
 unsigned protocols[]={ETH_P_IP,ETH_P_IPV6};
 for(unsigned i=0;i<2;i++) {
  s=(struct sk_buff){.protocol=protocols[i],.network_header=64,.mac_header=50,.transport_header=84};
  u64 flags=BPF_F_ADJ_ROOM_ENCAP_L3_IPV4|BPF_F_ADJ_ROOM_ENCAP_L2_ETH|BPF_F_ADJ_ROOM_ENCAP_L2(14);
  assert(!(flags & ~BPF_F_ADJ_ROOM_MASK));
  assert(bpf_skb_net_grow(&s,14,40,flags)==0);
  assert(s.encapsulation && s.inner_protocol==ETH_P_TEB);
  assert(s.inner_mac_header==50 && s.inner_network_header==64 && s.inner_transport_header==84);
  s=(struct sk_buff){.protocol=protocols[i],.network_header=64,.mac_header=50};
  assert(bpf_skb_net_grow(&s,14,40,BPF_F_ADJ_ROOM_ENCAP_L3_IPV4|BPF_F_ADJ_ROOM_ENCAP_L2_ETH|BPF_F_ADJ_ROOM_ENCAP_L2(13))==-EINVAL);
  assert(!s.encapsulation);
  assert(bpf_skb_net_grow(&s,14,10,flags)==-EINVAL);
  assert(!s.encapsulation);
  assert(bpf_skb_net_grow(&s,14,40,BPF_F_ADJ_ROOM_ENCAP_L3_IPV4)==0);
  assert(s.inner_protocol==protocols[i]);
 }
 puts("PASS: IPv4/IPv6 Ethernet encapsulation, short-header rejection and legacy protocol preservation");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(c)
 subprocess.run(['cc',str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
