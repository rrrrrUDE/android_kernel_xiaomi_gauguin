#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute production ARM64 JIT entry/tail/exit code on the host.
C-helper callbacks are modeled at the AAPCS boundary; no device pass implied.
"""
import ctypes as C
from pathlib import Path
import tempfile
import sys
sys.dont_write_bytecode = True
import test_arm64_trampoline as t

t.PRELUDE += r'''
#define MAX_TAIL_CALL_CNT 32
struct bpf_map { unsigned int max_entries; };
struct bpf_array { struct bpf_map map; void *ptrs[1]; };
'''
start = t.JIT.index('static int emit_bpf_tail_call')
end = t.JIT.index('#define BPF_FIXUP_OFFSET_MASK', start)
body = t.JIT[start:end]
t.HARNESS = body + t.HARNESS + r'''
int tail_image(int depth, int child, int action) {
 struct bpf_prog_aux a = { .stack_depth=depth, .func_idx=child };
 struct bpf_prog p = { .aux=&a, .is_func=true };
 struct jit_ctx ctx = { .image=image, .prog=&p };
 build_prologue(&ctx, false);
 if(action == 0) {
  emit(A64_MOV(1,A64_R(5),bpf2a64[TCALL_CNT]),&ctx);
  emit_a64_mov_i64(A64_R(10),0x100000,&ctx);
  emit(A64_BLR(A64_R(10)),&ctx);
 } else if(action == 1) {
  emit_bpf_tail_call(&ctx);
 } else if(action == 2) {
  emit(A64_MOV(1,A64_R(4),bpf2a64[TCALL_CNT]),&ctx);
  emit_a64_mov_i64(A64_R(10),0x200000,&ctx);
  emit(A64_BLR(A64_R(10)),&ctx);
 } else if(action == 3) {
  /* Target re-enters a callback after every successful tail call. */
  emit(A64_MOV(1,A64_R(4),bpf2a64[TCALL_CNT]),&ctx);
  emit_a64_mov_i64(A64_R(10),0x200000,&ctx);
  emit(A64_BLR(A64_R(10)),&ctx);
 }
 emit_a64_mov_i64(bpf2a64[BPF_REG_0],0,&ctx);
 build_epilogue(&ctx);
 return ctx.idx;
}
'''


def run(lib, mode, empty=False, invalid=False, initial_count=0):
    codes = {}
    def image(addr, child, action, depth):
        n = lib.tail_image(depth, child, action)
        codes[addr] = list((C.c_uint32 * n).from_address(lib.code()))
    # ordinary caller or C helper caller -> callback/subprog -> tail target
    image(0x400000, 0, 0 if mode == 'ordinary' else 2, 32)
    image(0x500000, 1, 1, 16)
    image(0x600000, 0, 3 if mode == 'callback' else 1, 64)
    regs = [0x770000 + i for i in range(31)]
    original = regs[:]
    sp, pc, end = 0x900000, 0x400000, 0xa00000
    regs[30], regs[1], regs[2] = end, 0x300000, int(invalid)
    mem = {0x300000: 1, 0x300008: 0 if empty else 0x310000,
           0x310008: 0x600000}
    calls, callbacks, transitions = [], 0, 0
    flags = (False, False)
    counter_ptr = None
    low = sp

    def read(r, stack=False):
        return (sp if stack else 0) if r == 31 else regs[r]
    def write(r, value, stack=False, sf=True):
        nonlocal sp
        value &= t.MASK if sf else 0xffffffff
        if r == 31:
            if stack:
                sp = value
        else:
            regs[r] = value

    for step in range(100000):
        if pc == end:
            assert not calls and sp == 0x900000
            assert regs[19:30] == original[19:30]
            count = mem[counter_ptr]
            if invalid:
                assert count == initial_count
            elif empty:
                assert count == initial_count + 1
            else:
                assert count == 33, count
                assert transitions == 33 - initial_count
            assert 0x900000 - low < 16384
            return transitions, callbacks, 0x900000 - low
        if pc == 0xb00000:
            saved, continuation = calls.pop()
            result = regs[0]
            regs[19:30] = saved
            for i in range(19):
                regs[i] = 0xbad000 + i
            regs[0], regs[1], regs[2] = result, 0x300000, int(invalid)
            pc = continuation
            continue
        address = next((b for b, code in codes.items()
                        if b <= pc < b + len(code) * 4), None)
        assert address is not None, hex(pc)
        insn = codes[address][(pc - address) // 4]
        rd, rn, rm = insn & 31, (insn >> 5) & 31, (insn >> 16) & 31
        nxt = pc + 4
        if insn == 0xd503201f:
            pass
        elif insn & 0x7f800000 in (0x12800000, 0x52800000, 0x72800000):
            sh, imm, op = ((insn >> 21) & 3) * 16, (insn >> 5) & 0xffff, insn & 0x7f800000
            value = (~(imm << sh) if op == 0x12800000 else
                     (read(rd) & ~(0xffff << sh)) | (imm << sh)
                     if op == 0x72800000 else imm << sh)
            write(rd, value, sf=bool(insn >> 31))
        elif insn & 0x7f000000 in (0x11000000, 0x51000000):
            imm = (insn >> 10) & 0xfff
            write(rd, read(rn, True) + (-imm if insn & (1 << 30) else imm),
                  True, bool(insn >> 31))
        elif insn & 0xffc00000 in (0xa9800000, 0xa8c00000):
            off, rt2 = t.signed((insn >> 15) & 127, 7) * 8, (insn >> 10) & 31
            assert rn == 31
            if insn & 0xffc00000 == 0xa9800000:
                sp += off
                mem[sp], mem[sp+8] = read(rd), read(rt2)
                # First prologue's freshly allocated counter slot.
                if rd == rt2 == 31 and counter_ptr is None:
                    counter_ptr = sp
                    mem[sp] = initial_count
            else:
                write(rd, mem[sp]); write(rt2, mem[sp+8]); sp += off
        elif insn & 0xffe0fc00 in (0xf8206800, 0xf8606800, 0xb8606800):
            addr = read(rn, True) + read(rm)
            if insn & (1 << 22):
                write(rd, mem[addr], sf=bool(insn >> 30 & 1))
            else:
                mem[addr] = read(rd)
        elif insn & 0xffc00000 == 0xd3400000:
            immr, imms = (insn >> 16) & 63, (insn >> 10) & 63
            if imms == 63:
                value = read(rn) >> immr
            elif imms + 1 == immr:
                value = read(rn) << (64 - immr)
            elif immr == 0:
                value = read(rn) & ((1 << (imms + 1)) - 1)
            else:
                raise AssertionError(hex(insn))
            write(rd, value)
        elif insn & 0x7f200000 in (0x0b000000, 0x6b000000):
            sf = bool(insn >> 31)
            left, right = read(rn), read(rm)
            if not sf:
                left &= 0xffffffff; right &= 0xffffffff
            if insn & (1 << 30):
                flags = (left >= right, left == right)
            else:
                write(rd, left + right, sf=sf)
        elif insn & 0xff000010 == 0x54000000:
            carry, zero = flags
            cond = insn & 15
            take = carry if cond == 2 else carry and not zero if cond == 8 else None
            assert take is not None
            if take:
                nxt = pc + t.signed((insn >> 5) & 0x7ffff, 19) * 4
        elif insn & 0x7f000000 == 0x34000000:
            if not read(rd):
                nxt = pc + t.signed((insn >> 5) & 0x7ffff, 19) * 4
        elif insn & 0xfffffc1f == 0xd65f0000:
            nxt = read(rn)
        elif insn & 0xfffffc1f == 0xd61f0000:
            nxt = read(rn); transitions += 1
        elif insn & 0xfffffc1f == 0xd63f0000:
            target = read(rn)
            if target == 0x100000:
                assert regs[5] == counter_ptr
                regs[30], nxt = pc + 4, 0x500000
            elif target == 0x200000:
                assert regs[4] == counter_ptr
                # AAPCS C code may freely use x26. Save/restore around its
                # invocation of callback with the explicitly passed x5.
                calls.append((regs[19:30], pc + 4))
                regs[26] = 0xdead
                regs[5], regs[30], nxt = regs[4], 0xb00000, 0x500000
                # Native map helper frames survive callback tail calls.
                sp -= 128
                calls[-1] += (sp,)
                callbacks += 1
            else:
                raise AssertionError(hex(target))
        else:
            raise AssertionError(f'{insn:08x} at {pc:x}')
        low = min(low, sp)
        # Model unwinding the native helper before its continuation.
        if nxt == 0xb00000 and calls:
            saved, continuation, helper_sp = calls.pop()
            assert sp == helper_sp
            sp += 128
            calls.append((saved, continuation))
        pc = nxt
    raise AssertionError('counter reset caused an unbounded cycle')


def main():
    with tempfile.TemporaryDirectory(prefix='arm64-callback-tail-') as directory:
        lib = t.build(Path(directory))
        for mode in ('ordinary', 'callback'):
            for count in (0, 1, 31, 32, 33):
                if count == 33:
                    run(lib, mode, initial_count=count)
                else:
                    run(lib, mode, initial_count=count)
                    run(lib, mode, empty=True, initial_count=count)
                    run(lib, mode, invalid=True, initial_count=count)
        print('PASS: production JIT shared counters, repeated C callbacks, '
              'tail-call limits, failure fallthrough and stack restoration')


if __name__ == '__main__':
    main()
