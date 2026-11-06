#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host execution test of the backport's actual ARM64 trampoline emitter.

Compiles the emitter and kernel instruction encoders on the host, then executes
its generated instructions in a deliberately small ARM64 interpreter. Kernel
RCU, ftrace patching, and CPU instruction-cache synchronization require device
selftests; this does not pretend to emulate them.
"""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
JIT = (ROOT / 'arch/arm64/net/bpf_jit_comp.c').read_text()
INSN = (ROOT / 'arch/arm64/kernel/insn.c').read_text()
HEADER = (ROOT / 'arch/arm64/include/asm/insn.h').read_text()
MACROS = (ROOT / 'arch/arm64/net/bpf_jit.h').read_text()
PRELUDE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint64_t u64; typedef int32_t s32; typedef int64_t s64;
typedef u32 __le32;
#define ENOTSUPP 524
#define __kprobes
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) ((~0UL << (l)) & (~0UL >> (63-(h))))
#define SZ_4 4
#define SZ_8 8
#define SZ_16 16
#define SZ_32 32
#define SZ_64 64
#define SZ_4K (1UL << 12)
#define SZ_64K (1UL << 16)
#define SZ_2M (1UL << 21)
#define SZ_1M (1UL << 20)
#define SZ_128M (1UL << 27)
#define AARCH64_BREAK_FAULT 0xd4202000
#define pr_err(...) fprintf(stderr, __VA_ARGS__)
#define pr_err_once(...) pr_err(__VA_ARGS__)
#define cpu_to_le32(x) (x)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define round_down(x,y) ((x) & ~((y)-1))
#define fls64(x) ((x) ? 64 - __builtin_clzll(x) : 0)
#define AARCH64_INSN_SF_BIT BIT(31)
#define AARCH64_INSN_N_BIT BIT(22)
#define AARCH64_INSN_LSL_12 BIT(22)
#define MAX_BPF_FUNC_ARGS 12
#define BPF_MAX_TRAMP_PROGS 40
#define BPF_TRAMP_FENTRY 0
#define BPF_TRAMP_FEXIT 1
#define BPF_TRAMP_MODIFY_RETURN 2
#define BPF_TRAMP_MAX 3
#define BPF_TRAMP_F_RESTORE_REGS 1
#define BPF_TRAMP_F_CALL_ORIG 2
#define BPF_TRAMP_F_SKIP_FRAME 4
#define BPF_TRAMP_F_IP_ARG 8
#define IS_ENABLED(x) 1
struct bpf_prog_aux { bool sleepable; unsigned int stack_depth, func_idx; };
struct bpf_prog { bool jited, is_func; void *bpf_func; void *insnsi;
                  struct bpf_prog_aux *aux; };
struct bpf_tramp_progs { struct bpf_prog *progs[40]; int nr_progs; };
struct btf_func_model { u8 ret_size, nr_args, arg_size[12]; };
struct bpf_tramp_image { void *ip_after_call, *ip_epilogue; };
enum bpf_text_poke_type { BPF_MOD_CALL, BPF_MOD_JUMP };
static int text_mutex;
#define cpus_read_lock() do {} while (0)
#define cpus_read_unlock() do {} while (0)
#define mutex_lock(p) do {} while (0)
#define mutex_unlock(p) do {} while (0)
int aarch64_insn_patch_text(void *addrs[], u32 insns[], int cnt) {
 for(int i=0;i<cnt;i++) memcpy(addrs[i], &insns[i], 4);
 return 0;
}
struct bpf_prog;
void __bpf_prog_enter(void) {} void __bpf_prog_exit(void) {}
void __bpf_prog_enter_sleepable(void) {} void __bpf_prog_exit_sleepable(void) {}
void __bpf_tramp_enter(void) {} void __bpf_tramp_exit(void) {}
static unsigned long ftrace_location(unsigned long ip) { return ip + 4; }
static bool is_bpf_text_address(unsigned long ip) { return true; }
int aarch64_insn_read(void *p, u32 *v) { memcpy(v,p,4); return 0; }
#define flush_icache_range(a,b) do {} while (0)
'''
HARNESS = r'''
static u32 image[4096];
static struct bpf_tramp_image im;
static struct bpf_prog_aux aux[4];
static struct bpf_prog progs[4];
static struct bpf_tramp_progs tp[3];
static struct btf_func_model model;
int prologue(int depth, int classic) {
 struct bpf_prog_aux a = { .stack_depth=depth };
 struct bpf_prog p = { .aux=&a };
 struct jit_ctx ctx = { .image=image, .prog=&p };
 int ret=build_prologue(&ctx, classic);
 return ret ? ret : ctx.idx;
}
int tail_offset(void) { return PROLOGUE_OFFSET; }
void patch_site(void) {
 image[0]=A64_MOV(1,A64_R(9),A64_LR);
 image[1]=aarch64_insn_gen_nop(); image[2]=0x12345678;
}
void *hook(int n) {
 void *hooks[] = { __bpf_prog_enter, __bpf_prog_exit,
 __bpf_prog_enter_sleepable, __bpf_prog_exit_sleepable,
 __bpf_tramp_enter, __bpf_tramp_exit };
 return hooks[n];
}
void *code(void) { return image; }
long teardown(int epilogue) {
 return ((u32 *)(epilogue ? im.ip_epilogue : im.ip_after_call) - image);
}
int generate(int nargs, int flags, int mods, int sleepable, int interpreted,
             int capacity) {
 int i;
 memset(tp,0,sizeof(tp)); memset(&im,0,sizeof(im));
 model.nr_args=nargs; model.ret_size=8;
 for(i=0;i<12;i++) model.arg_size[i] = i % 4 == 0 ? 1 :
                                      i % 4 == 1 ? 2 :
                                      i % 4 == 2 ? 4 : 8;
 for(i=0;i<4;i++) {
  aux[i].sleepable=sleepable; progs[i].aux=&aux[i];
  progs[i].jited=!interpreted; progs[i].bpf_func=(void *)(0x100000UL+i*16);
  progs[i].insnsi=(void *)(0x200000UL+i*16);
 }
 tp[0].nr_progs=1; tp[0].progs[0]=&progs[0];
 if(flags & 2) { tp[1].nr_progs=1; tp[1].progs[0]=&progs[3]; }
 tp[2].nr_progs=mods;
 for(i=0;i<mods;i++) tp[2].progs[i]=&progs[i+1];
 return prepare_trampoline(&im,image,(char *)image+capacity,&model,flags,tp,
                           (void *)0x300000UL);
}
'''


def build(directory):
    # Use the production encoders, not copies of their instruction constants.
    header = HEADER.replace('#include <linux/types.h>', '')
    macros = MACROS.replace('#include <asm/insn.h>', '')
    encoders = INSN[INSN.index('static int __kprobes aarch64_get_imm_shift_mask'):
                    INSN.index('s32 aarch64_get_branch_offset')]
    emitter = JIT[JIT.index('struct jit_ctx {'):JIT.index('/* Struct_ops callbacks')]
    poke = JIT[JIT.index('int bpf_arch_text_poke('):]
    registers = '\n'.join(f'#define BPF_REG_{i} {i}' for i in range(10))
    registers += '\n#define BPF_REG_FP 10\n#define BPF_REG_AX 11\n#define MAX_BPF_JIT_REG 12\n'
    mapping = JIT[JIT.index('#define TMP_REG_1'):JIT.index('struct jit_ctx {')]
    prologue = JIT[JIT.index('/* Stack must be multiples of 16B */'):
                   JIT.index('static int emit_bpf_tail_call')]
    source = (PRELUDE + header + macros + encoders + registers + mapping +
              emitter + prologue + poke + HARNESS)
    path = directory / 'test.c'
    path.write_text(source)
    output = directory / 'test.so'
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-shared', '-fPIC', '-O0',
                    '-Werror=implicit-function-declaration', str(path),
                    '-o', str(output)], check=True)
    lib = C.CDLL(str(output))
    lib.code.restype = C.c_void_p
    lib.hook.restype = C.c_void_p
    lib.teardown.restype = C.c_long
    lib.bpf_arch_text_poke.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.c_void_p]
    return lib


MASK = (1 << 64) - 1


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def execute(lib, nargs, flags, mods, sleepable, interpreted, results, detach=False):
    size = lib.generate(nargs, flags, mods, sleepable, interpreted, 4096)
    assert size > 0, size
    base = lib.code()
    words = list((C.c_uint32 * (size // 4)).from_address(base))
    if detach:
        site, end = lib.teardown(0), lib.teardown(1)
        words[site] = 0x14000000 | ((end - site) & 0x3ffffff)
    regs = [0xfeed000000000000 + i for i in range(31)]
    initial = regs[:]
    sp = original_sp = 0x800000
    parent, resume = 0x400000, 0x500000
    regs[9], regs[30] = parent, resume
    mem = {}
    args = initial[:8] + [0xdead000000001000 + i for i in range(8, nargs)]
    for i in range(8, nargs):
        mem[sp + (i - 8) * 8] = args[i]
    expected = [v & ((1 << ([1, 2, 4, 8][i % 4] * 8)) - 1)
                for i, v in enumerate(args[:nargs])]
    events, refs, locks = [], 0, 0
    hooks = {lib.hook(i): i for i in range(6)}
    pc = base

    def read(reg, use_sp=False):
        return (sp if use_sp else 0) if reg == 31 else regs[reg]

    for step in range(10000):
        if pc in (parent, resume):
            assert pc == (parent if flags & 2 else resume)
            assert sp == original_sp
            assert regs[19:30] == initial[19:30]
            assert refs == locks == 0
            if flags & 2:
                modified = next((x for x in results if x), None)
                assert regs[0] == (modified if modified is not None else 0xabcdef)
                assert ('orig' in events) == (modified is None)
                assert ('exit' in events) == (not detach or modified is not None)
            else:
                assert regs[:9] == initial[:9]
            return
        assert base <= pc < base + size, hex(pc)
        insn = words[(pc - base) // 4]
        nxt = pc + 4
        rd, rn, rm = insn & 31, (insn >> 5) & 31, (insn >> 16) & 31
        if insn == 0xd503201f:
            pass
        elif insn & 0x7f800000 in (0x12800000, 0x52800000, 0x72800000):
            shift, imm = ((insn >> 21) & 3) * 16, (insn >> 5) & 0xffff
            op = insn & 0x7f800000
            value = (~(imm << shift) if op == 0x12800000 else
                     (regs[rd] & ~(0xffff << shift)) | (imm << shift)
                     if op == 0x72800000 else imm << shift)
            regs[rd] = value & (MASK if insn >> 31 else 0xffffffff)
        elif insn & 0x7f000000 in (0x11000000, 0x51000000):
            imm = (insn >> 10) & 0xfff
            value = read(rn, True) + (-imm if insn & (1 << 30) else imm)
            if rd == 31:
                sp = value
            else:
                regs[rd] = value & MASK
        elif insn & 0xffc00000 in (0xa9800000, 0xa8c00000):
            off, rt2 = signed((insn >> 15) & 127, 7) * 8, (insn >> 10) & 31
            assert rn == 31
            if insn & 0xffc00000 == 0xa9800000:
                sp += off
                mem[sp], mem[sp + 8] = read(rd), read(rt2)
            else:
                if rd != 31:
                    regs[rd] = mem[sp]
                if rt2 != 31:
                    regs[rt2] = mem[sp + 8]
                sp += off
        elif insn & 0xffe0fc00 in (0xf8206800, 0xf8606800):
            addr = read(rn, True) + read(rm)
            if insn & (1 << 22):
                if rd != 31:
                    regs[rd] = mem[addr]
            else:
                mem[addr] = read(rd)
        elif insn & 0xffc00000 == 0xd3400000:
            assert ((insn >> 16) & 63) == 0
            regs[rd] = read(rn) & ((1 << (((insn >> 10) & 63) + 1)) - 1)
        elif insn & 0xff000000 == 0xb5000000:
            if read(rd):
                nxt = pc + signed((insn >> 5) & 0x7ffff, 19) * 4
        elif insn & 0xfc000000 == 0x14000000:
            nxt = pc + signed(insn & 0x3ffffff, 26) * 4
        elif insn & 0xfffffc1f == 0xd65f0000:
            nxt = read(rn)
        elif insn & 0xfffffc1f == 0xd63f0000:
            target = read(rn)
            assert sp % 16 == 0
            result = 0
            if target in hooks:
                kind = hooks[target]
                if kind in (0, 2):
                    locks += 1
                    result = 0x123456 if kind == 0 else 0xbad
                elif kind in (1, 3):
                    locks -= 1
                    if kind == 1:
                        assert regs[1] == 0x123456
                elif kind == 4:
                    refs += 1
                else:
                    refs -= 1
            elif 0x100000 <= target < 0x100040:
                which = (target - 0x100000) // 16
                ctx = regs[0]
                assert [mem[ctx + i * 8] for i in range(nargs)] == expected
                if flags & 8:
                    assert mem[ctx - 8] == 0x300000
                if interpreted:
                    assert regs[1] == 0x200000 + which * 16
                if which in (1, 2):
                    events.append('mod')
                    assert mem[ctx + nargs * 8] == 0
                    result = results[which - 1]
                elif which == 3:
                    events.append('exit')
                    assert mem[ctx + nargs * 8] == next(
                        (x for x in results if x), 0xabcdef)
                else:
                    events.append('entry')
            elif target == 0x300008:
                events.append('orig')
                assert regs[:9] == initial[:9]
                assert [mem[sp + i * 8] for i in range(max(nargs - 8, 0))] == args[8:]
                result = 0xabcdef
            else:
                raise AssertionError(hex(target))
            # Simulate a hostile but AAPCS-conforming callee.
            for i in range(19):
                regs[i] = 0xbad0000000000000 + i
            regs[0], regs[30] = result & MASK, nxt
        else:
            raise AssertionError(f'unsupported instruction {insn:08x} at {pc-base:x}')
        pc = nxt
    raise AssertionError('instruction limit')


def main():
    with tempfile.TemporaryDirectory(prefix='arm64-trampoline-') as tmp:
        lib = build(Path(tmp))
        count = 0
        for nargs in (0, 1, 5, 8, 9, 11):
            for sleepable in (0, 1):
                for interpreted in (0, 1):
                    for ip in (0, 8):
                        execute(lib, nargs, 1 | ip, 0, sleepable, interpreted, [])
                        count += 1
                        for results in ([], [0], [0, 0], [17, 0], [0, MASK - 4]):
                            for detach in (False, True):
                                execute(lib, nargs, 6 | ip, len(results), sleepable,
                                        interpreted, results, detach)
                                count += 1
        assert lib.generate(12, 1, 0, 0, 0, 4096) == -524
        assert lib.generate(1, 7, 0, 0, 0, 4096) == -524
        assert lib.generate(1, 1, 0, 0, 0, 4) == -7
        # The same new entry instructions must be included in tail-call
        # offsets; validate the production prologue, not a copied constant.
        for depth in (0, 1, 16, 128, 512):
            assert lib.prologue(depth, 0) == lib.tail_offset() + 1
            words = (C.c_uint32 * 16).from_address(lib.code())
            assert words[0] == 0x910003c9 and words[1] == 0xd503201f
            # Tail calls jump directly to the stack allocation instruction.
            assert words[lib.tail_offset()] == 0xd10003ff | (((depth + 15) & ~15) << 10)
            assert lib.prologue(depth, 1) == lib.tail_offset() + 1
        # Exercise production compare-and-replace for freplace and fentry.
        for kind in (0, 1):
            lib.patch_site()
            base = lib.code()
            words = (C.c_uint32 * 3).from_address(base)
            mov, nop = words[0], words[1]
            poke = lib.bpf_arch_text_poke
            target, replacement = base + 64, base + 128
            assert poke(base, kind, None, target) == 0
            assert words[0] == mov and words[2] == 0x12345678
            expected = (0x94000000 if kind == 0 else 0x14000000) | 15
            assert words[1] == expected
            assert poke(base, kind, None, replacement) == -16
            assert words[1] == expected
            assert poke(base, kind, target, replacement) == 0
            assert poke(base, kind, replacement, None) == 0
            assert words[1] == nop
            assert poke(base, kind, None, base + 2) == -22
            assert poke(base, kind, None, base + 4 + (1 << 27)) == -34
            assert poke(base, kind, None, base + 4 - (1 << 27)) == 0
            assert poke(base, kind, base + 4 - (1 << 27), None) == 0
            # Teardown patches use the NOP itself, not a function entry.
            assert poke(base + 4, 1, None, target) == 0
            assert poke(base + 4, 1, target, None) == 0
        print(f'PASS: {count} emitted-code execution cases, invalid inputs, '
              'and CALL/JUMP patch lifecycle and range checks')


if __name__ == '__main__':
    main()
