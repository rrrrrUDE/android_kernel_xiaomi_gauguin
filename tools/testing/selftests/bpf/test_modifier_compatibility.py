#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run production verifier type compatibility and spill predicates on the host.
This is not an on-device verifier load test.
"""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
h = (root / 'include/linux/bpf.h').read_text()
s = (root / 'kernel/bpf/verifier.c').read_text()
def enum(name):
    a = h.index('enum ' + name + ' {')
    return h[a:h.index('\n};', a) + 3]
def function(name):
    # Locate definition independently of return type.
    import re
    m = re.search(r'static [^\n]+ ' + name + r'\(', s)
    a = m.start()
    return s[a:s.index('\n}', a) + 2]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define BIT(x) (1U << (x))
#define BPF_BASE_TYPE_BITS 8
#define BPF_BASE_TYPE_MASK 255
#define BPF_TYPE_LIMIT 1023
#define base_type(t) ((t) & BPF_BASE_TYPE_MASK)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define verbose(...) do {} while (0)
typedef unsigned int u32;
struct tnum { unsigned long value; };
struct bpf_reg_state { enum_placeholder unused; };
'''.replace('struct bpf_reg_state { enum_placeholder unused; };','')
pre += enum('bpf_type_flag') + enum('bpf_arg_type') + enum('bpf_reg_type')
pre += r'''
struct bpf_reg_state { enum bpf_reg_type type; int off; u32 btf_id; struct tnum var_off; };
struct bpf_verifier_env { int log; struct bpf_reg_state regs[11]; };
#define cur_regs(env) ((env)->regs)
static int tnum_is_const(struct tnum x) { return 1; }
static int btf_struct_ids_match(void *l, int off, u32 id, u32 want) { return id == want; }
static const char *kernel_type_name(u32 id) { return "stub"; }
static const char *reg_type_str(struct bpf_verifier_env *e, int type) { return "stub"; }
'''
a = s.index('struct bpf_reg_types {'); b = s.index('static int check_func_arg(', a)
pre += s[a:b] + function('is_spillable_regtype')
pre += r'''
int main(void) {
 struct bpf_verifier_env e = {};
 unsigned int nullable, readonly, argnull, argreadonly;
 for (nullable=0; nullable<2; nullable++)
 for (readonly=0; readonly<2; readonly++)
 for (argnull=0; argnull<2; argnull++)
 for (argreadonly=0; argreadonly<2; argreadonly++) {
  unsigned int flags=(nullable ? PTR_MAYBE_NULL:0)|(readonly ? MEM_RDONLY:0);
  unsigned int aflags=(argnull ? PTR_MAYBE_NULL:0)|(argreadonly ? MEM_RDONLY:0);
  e.regs[1].type=PTR_TO_MEM|flags;
  assert((check_reg_type(&e,1,ARG_PTR_TO_MEM|aflags,0)==0) ==
         ((!nullable || argnull) && (!readonly || argreadonly)));
  assert(is_spillable_regtype(e.regs[1].type));
  e.regs[1].type=PTR_TO_BUF|flags;
  assert((check_reg_type(&e,1,ARG_PTR_TO_MEM|aflags,0)==0) ==
         ((!nullable || argnull) && (!readonly || argreadonly)));
  assert(is_spillable_regtype(e.regs[1].type));
 }
 e.regs[1].type=PTR_TO_MEM|MEM_RDONLY;
 assert(check_reg_type(&e,1,ARG_PTR_TO_ALLOC_MEM,0)==-EACCES);
 assert(check_reg_type(&e,1,255,0)==-EFAULT);
 puts("PASS: 32 production nullable/readonly compatibility cases and spill preservation");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bpf-compat-') as directory:
    p = Path(directory)
    (p/'test.c').write_text(pre)
    subprocess.run([os.environ.get('HOSTCC','cc'), '-Wno-unused-function',
                    str(p/'test.c'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
