#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute production BTF global argument matching and preparation with host stubs."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
s = (root/'kernel/bpf/btf.c').read_text()
def function(name):
    a=s.index('int '+name+'(')
    return s[a:s.index('\n}',a)+2]
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
#define BPF_FUNC_GLOBAL 1
#define BTF_FUNC_GLOBAL 1
#define BPF_LOG_LEVEL 1
#define BTF_INFO_KIND(x) (x)
#define BPF_PROG_TYPE_EXT 2
#define PTR_TO_CTX 1
#define SCALAR_VALUE 2
#define PTR_TO_MEM 3
#define PTR_MAYBE_NULL 256
#define bpf_log(...) ((void)0)
#define IS_ERR(x) ((intptr_t)(x)<0)
#define PTR_ERR(x) ((long)(x))
enum bpf_prog_type { NORMAL, UNUSED, EXT };
struct tnum { unsigned long value, mask; };
struct bpf_reg_state { unsigned type, mem_size, id; struct tnum var_off; };
struct btf_type { u32 name_off, type, info, size, nargs; };
struct btf_param { u32 name_off,type; };
struct btf { int unused; };
struct info { u32 type_id; };
struct info_aux { bool unreliable; int linkage; };
struct bpf_prog;
struct aux { struct btf *btf; struct info *func_info; struct info_aux *func_info_aux; struct bpf_prog *dst_prog; };
struct bpf_prog { enum bpf_prog_type type; struct aux *aux; };
struct bpf_verifier_log { unsigned level; };
struct bpf_verifier_env { struct bpf_prog *prog; struct bpf_verifier_log log; unsigned id_gen; };
static struct btf_type types[8];
static struct { struct btf_type proto; struct btf_param args[5]; } proto;
static const char *btf_kind_str[]={"void","int","ptr","struct","func","proto"};
#define btf_type_is_int(t) ((t)->info==1)
#define btf_type_is_enum(t) 0
#define btf_type_is_ptr(t) ((t)->info==2)
#define btf_type_is_func(t) ((t)->info==4)
#define btf_type_is_func_proto(t) ((t)->info==5)
#define btf_type_is_modifier(t) 0
#define btf_type_vlen(t) ((t)->nargs)
static const struct btf_type *btf_type_by_id(struct btf *b,u32 id) { return id==2 ? &proto.proto : &types[id]; }
static const struct btf_type *btf_type_skip_modifiers(struct btf *b,u32 id,void *out) { return btf_type_by_id(b,id); }
static const char *btf_name_by_offset(struct btf *b,u32 id) { return "test"; }
static const char *btf_type_str(const struct btf_type *t) { return "test"; }
static const struct btf_type *btf_resolve_size(struct btf *b,const struct btf_type *t,u32 *size) { if(!t->size) return (void *)-EINVAL; *size=t->size; return t; }
static bool btf_get_prog_ctx_type(struct bpf_verifier_log *l,struct btf *b,const struct btf_type *t,enum bpf_prog_type p,int i) { return t->type==6; }
static int tnum_is_const(struct tnum t) { return !t.mask; }
static int check_ctx_reg(struct bpf_verifier_env *e,struct bpf_reg_state *r,int n) { return 0; }
static int check_mem_reg(struct bpf_verifier_env *e,struct bpf_reg_state *r,u32 n,u32 size) { return r->type==SCALAR_VALUE ? (r->var_off.value||r->var_off.mask ? -EINVAL:0) : ((r->type&255)==PTR_TO_MEM && r->mem_size>=size ? 0:-EINVAL); }
'''
c+=function('btf_check_subprog_arg_match')+function('btf_prepare_func_args')
c+=r'''
int main(void) {
 struct btf b={}; struct info info={1}; struct info_aux ia={.linkage=BTF_FUNC_GLOBAL};
 struct aux aux={.btf=&b,.func_info=&info,.func_info_aux=&ia};
 struct bpf_prog p={.aux=&aux}; struct bpf_verifier_env e={.prog=&p};
 struct bpf_reg_state regs[6]={};
 types[1]=(struct btf_type){.info=4,.type=2};
 proto.proto=(struct btf_type){.info=5,.type=3,.nargs=1};
 proto.args[0].type=4;
 types[3]=(struct btf_type){.info=1,.size=4};
 types[4]=(struct btf_type){.info=2,.type=5};
 types[5]=(struct btf_type){.info=3,.size=16};
 assert(!btf_prepare_func_args(&e,0,regs));
 assert(regs[1].type==(PTR_TO_MEM|PTR_MAYBE_NULL) && regs[1].mem_size==16 && regs[1].id);
 assert(!btf_check_subprog_arg_match(&e,0,regs));
 regs[1].mem_size=15;
 assert(btf_check_subprog_arg_match(&e,0,regs)==-EINVAL && ia.unreliable);
 ia.unreliable=false; regs[1]=(struct bpf_reg_state){.type=SCALAR_VALUE};
 assert(!btf_check_subprog_arg_match(&e,0,regs));
 regs[1].var_off.value=1;
 assert(btf_check_subprog_arg_match(&e,0,regs)==-EINVAL);
 ia.unreliable=false; types[5].size=0;
 assert(btf_prepare_func_args(&e,0,regs)==-EINVAL);
 types[4].type=6; types[6].info=3;
 assert(!btf_prepare_func_args(&e,0,regs) && regs[1].type==PTR_TO_CTX);
 assert(!btf_check_subprog_arg_match(&e,0,regs));
 puts("PASS: production global BTF memory preparation, size bounds, NULL and context arguments");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.c';p.write_text(c)
    subprocess.run(['cc',str(p),'-o',d+'/test'],check=True)
    subprocess.run([d+'/test'],check=True)
