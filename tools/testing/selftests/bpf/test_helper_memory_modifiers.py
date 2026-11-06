#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute production helper memory dispatch with stubbed bounds checks."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
s = (root / 'kernel/bpf/verifier.c').read_text()
h = (root / 'include/linux/bpf.h').read_text()
def enum(name):
    a = h.index('enum ' + name + ' {')
    return h[a:h.index('\n};', a) + 3]
a = s.index('static int check_helper_mem_access(')
b = s.index('\nint check_kfunc_mem_access', a)
c = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#define BIT(x) (1U << (x))
#define BPF_BASE_TYPE_BITS 8
#define BPF_BASE_TYPE_MASK 255
#define BPF_TYPE_LIMIT 1023
#define base_type(t) ((t) & BPF_BASE_TYPE_MASK)
#define verbose(...) ((void)0)
'''
c += enum('bpf_type_flag') + enum('bpf_reg_type')
c += r'''
struct map { int key_size; };
struct bpf_reg_state { int type, off, mem_size; struct map *map_ptr; };
struct aux { unsigned max_rdonly_access, max_rdwr_access; };
struct prog { struct aux *aux; };
struct bpf_verifier_env { struct bpf_reg_state regs[11]; struct prog *prog; };
struct bpf_call_arg_meta { bool raw_mode, mem_rdonly; };
#define cur_regs(e) ((e)->regs)
#define BPF_WRITE 1
#define BPF_READ 0
#define ACCESS_HELPER 0
#define check_packet_access(...) 0
#define check_map_access_type(...) 0
#define check_map_access(...) 0
#define check_mem_region_access(e,r,o,n,z,zero) ((n) <= (z) ? 0 : -EACCES)
#define check_stack_range_initialized(...) 0
#define register_is_null(r) ((r)->type == SCALAR_VALUE)
static int check_buffer_access(struct bpf_verifier_env *e, struct bpf_reg_state *r,
 int no, int off, int size, bool zero, const char *name, unsigned *max)
{ *max = size; return 0; }
'''
c += s[a:b]
c += r'''
int main(void) {
 struct aux aux = {};
 struct prog prog = { &aux };
 struct bpf_verifier_env e = { .prog = &prog };
 struct bpf_call_arg_meta m = { .mem_rdonly = true };
 e.regs[1].type = PTR_TO_MEM | MEM_RDONLY;
 e.regs[1].mem_size = 16;
 assert(check_helper_mem_access(&e,1,8,false,&m)==0);
 assert(check_helper_mem_access(&e,1,17,false,&m)==-EACCES);
 m.mem_rdonly=false;
 assert(check_helper_mem_access(&e,1,8,false,&m)==-EACCES);
 e.regs[1].type = PTR_TO_BUF | MEM_RDONLY;
 m.mem_rdonly=true;
 assert(check_helper_mem_access(&e,1,8,false,&m)==0);
 assert(aux.max_rdonly_access==8 && aux.max_rdwr_access==0);
 m.mem_rdonly=false;
 assert(check_helper_mem_access(&e,1,8,false,&m)==-EACCES);
 e.regs[1].type=PTR_TO_BUF;
 assert(check_helper_mem_access(&e,1,12,false,&m)==0);
 assert(aux.max_rdwr_access==12);
 puts("PASS: helper memory modifier dispatch and access checks");
}
'''
# Variable-size input must retain readonly metadata when disabling raw mode.
a = s.index('} else if (arg_type_is_mem_size(arg_type)) {')
b = s.index('} else if (arg_type_is_alloc_size', a)
assert 'meta = NULL' not in s[a:b]
assert 'meta->raw_mode = false' in s[a:b]
with tempfile.TemporaryDirectory() as d:
    src = Path(d) / 'test.c'
    src.write_text(c)
    subprocess.run(['cc', '-Wno-unused-function', str(src), '-o', d+'/test'], check=True)
    subprocess.run([d+'/test'], check=True)
