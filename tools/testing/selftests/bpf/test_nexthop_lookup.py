#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise production common IPv4 FIB nexthop eligibility rules."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[4]
src=(root/'net/ipv4/fib_trie.c').read_text()
a=src.index('bool fib_lookup_good_nhc(')
b=src.index('/* should be called with rcu_read_lock */',a)
code=r'''
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define RTNH_F_DEAD 1
#define RTNH_F_LINKDOWN 16
#define FIB_LOOKUP_IGNORE_LINKSTATE 4
#define FLOWI_FLAG_SKIP_NH_OIF 4
struct in_device { bool ignore; };
struct net_device { struct in_device *in; };
struct fib_nh_common { unsigned nhc_flags; int nhc_oif; struct net_device *nhc_dev; };
struct flowi4 { unsigned flowi4_flags; int flowi4_oif; };
#define __in_dev_get_rcu(d) ((d)->in)
#define IN_DEV_IGNORE_ROUTES_WITH_LINKDOWN(d) ((d)->ignore)
'''+src[a:b]+r'''
int main(void) {
 struct in_device in={}; struct net_device dev={.in=&in};
 struct fib_nh_common nh={.nhc_dev=&dev,.nhc_oif=7}; struct flowi4 fl={};
 assert(fib_lookup_good_nhc(&nh,0,&fl));
 fl.flowi4_oif=8; assert(!fib_lookup_good_nhc(&nh,0,&fl));
 fl.flowi4_flags=FLOWI_FLAG_SKIP_NH_OIF;
 assert(fib_lookup_good_nhc(&nh,0,&fl));
 fl.flowi4_flags=0; fl.flowi4_oif=7; nh.nhc_flags=RTNH_F_LINKDOWN;
 assert(fib_lookup_good_nhc(&nh,0,&fl));
 in.ignore=true; assert(!fib_lookup_good_nhc(&nh,0,&fl));
 assert(fib_lookup_good_nhc(&nh,FIB_LOOKUP_IGNORE_LINKSTATE,&fl));
 nh.nhc_flags|=RTNH_F_DEAD;
 assert(!fib_lookup_good_nhc(&nh,FIB_LOOKUP_IGNORE_LINKSTATE,&fl));
 nh.nhc_flags=RTNH_F_LINKDOWN; dev.in=NULL;
 assert(fib_lookup_good_nhc(&nh,0,&fl));
 puts("PASS: production nexthop lookup dead/linkdown/oif filtering and override flags");
}
'''
with tempfile.TemporaryDirectory(prefix='nh-lookup-') as temp:
 source=Path(temp)/'test.c';binary=Path(temp)/'test';source.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Werror',str(source),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
