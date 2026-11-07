#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the production exception PMTU updater and its locking contract."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
src = (root / 'net/ipv4/route.c').read_text()
a = src.index('void fnhe_update_pmtu(')
b = src.index('\nstatic void fnhe_flush_routes', a)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef unsigned u32;
struct fib_nh_exception { u32 fnhe_pmtu; bool fnhe_mtu_locked; };
static int fnhe_lock, held, locks;
#define spin_lock_bh(p) do { assert((p)==&fnhe_lock && !held); held=1; ++locks; } while(0)
#define spin_unlock_bh(p) do { assert((p)==&fnhe_lock && held); held=0; } while(0)
'''+src[a:b]+r'''
int main(void) {
 struct fib_nh_exception e={.fnhe_pmtu=1400,.fnhe_mtu_locked=true};
 fnhe_update_pmtu(&e,1500,1400);
 assert(e.fnhe_pmtu==1400 && e.fnhe_mtu_locked);
 fnhe_update_pmtu(&e,1400,1500);
 assert(e.fnhe_pmtu==1400 && !e.fnhe_mtu_locked);
 e.fnhe_mtu_locked=true;
 fnhe_update_pmtu(&e,1280,1500);
 assert(e.fnhe_pmtu==1280 && !e.fnhe_mtu_locked);
 fnhe_update_pmtu(&e,1500,1400);
 assert(e.fnhe_pmtu==1280);
 fnhe_update_pmtu(&e,1500,1280);
 assert(e.fnhe_pmtu==1500);
 fnhe_update_pmtu(&e,1300,1400);
 assert(e.fnhe_pmtu==1300 && !held && locks==6);
 puts("PASS: production nexthop PMTU lock/unlock, locked MTU and increase/decrease rules");
}
'''
with tempfile.TemporaryDirectory(prefix='nh-mtu-') as temp:
    source = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    source.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', str(source),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
