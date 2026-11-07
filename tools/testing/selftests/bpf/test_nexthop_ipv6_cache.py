#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check ownership isolation of the production IPv6 per-CPU cache."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[4]
s=(root/'net/ipv6/route.c').read_text();a=s.index('static struct rt6_info *rt6_get_pcpu_route(');b=s.index('static struct rt6_info *rt6_make_pcpu_route(',a)
code=r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#define READ_ONCE(x) (x)
#define rcu_access_pointer(x) (x)
#define this_cpu_ptr(x) (x)
#define xchg(p,v) ({__typeof__(*(p)) old=*(p);*(p)=(v);old;})
struct fib6_info {int id;};
struct dst_entry {int refs,devrefs;};
struct rt6_info {struct fib6_info *from;struct dst_entry dst;};
struct fib6_nh {struct rt6_info **rt6i_pcpu;};
static void dst_dev_put(struct dst_entry *d) {assert(d->devrefs>0);d->devrefs--;}
static void dst_release(struct dst_entry *d) {assert(d->refs>0);d->refs--;}
static void ip6_hold_safe(void *n,struct rt6_info **r,bool b) {(*r)->dst.refs++;}
'''+s[a:b]+r'''
int main(void) {
 struct fib6_info first={1},second={2};
 struct rt6_info cached={.from=&first,.dst={1,1}},*slot=&cached;
 struct fib6_nh nh={&slot};
 assert(rt6_get_pcpu_route(&first,&nh)==&cached&&slot==&cached&&cached.dst.refs==2);
 dst_release(&cached.dst);
 assert(!rt6_get_pcpu_route(&second,&nh)&&!slot&&!cached.dst.refs&&!cached.dst.devrefs);
 assert(!rt6_get_pcpu_route(&second,&nh));
 cached.from=NULL;cached.dst=(struct dst_entry){1,1};slot=&cached;
 assert(!rt6_get_pcpu_route(&first,&nh)&&!slot&&!cached.dst.refs&&!cached.dst.devrefs);
 puts("PASS: production IPv6 shared per-CPU cache reuse, foreign-prefix eviction, detached FIB and reference balance");
}
'''
with tempfile.TemporaryDirectory(prefix='nh-v6-cache-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Werror',str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
