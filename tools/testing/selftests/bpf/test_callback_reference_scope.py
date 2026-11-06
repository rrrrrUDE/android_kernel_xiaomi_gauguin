#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute actual verifier reference-scope routines with synthetic call frames.
This host test does not replace loading programs on the patched kernel.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'kernel/bpf/verifier.c').read_text()

def function(name):
    start = source.index('static int ' + name + '(')
    end = source.index('\n}', start) + 2
    return source[start:end]

preamble = r'''
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
struct bpf_reference_state { int id, insn_idx, callback_ref; };
struct bpf_func_state {
 unsigned int frameno, callback_frameno;
 bool in_callback_fn;
 int acquired_refs;
 struct bpf_reference_state refs[16];
};
struct bpf_verifier_env { struct bpf_func_state *state; int id_gen; };
#define cur_func(env) ((env)->state)
#define verbose(...) do {} while (0)
static int realloc_reference_state(struct bpf_func_state *s, int n, bool copy)
{ s->acquired_refs=n; return 0; }
'''
tests = r'''
int main(void) {
 struct bpf_func_state main = {}, outer = {}, sub = {}, inner = {};
 struct bpf_verifier_env env = { .state=&main };
 int parent=acquire_reference_state(&env, 1);
 outer=main; outer.frameno=1; outer.callback_frameno=1;
 outer.in_callback_fn=true; env.state=&outer;
 assert(release_reference_state(&outer,parent)==-EINVAL);
 assert(check_reference_leak(&env)==0); /* inherited ref isn't ours */
 int a=acquire_reference_state(&env,2);
 assert(check_reference_leak(&env)==-EINVAL);
 sub=outer; sub.frameno=2; sub.in_callback_fn=false; env.state=&sub;
 assert(check_reference_leak(&env)==0); /* may return live refs */
 assert(check_reference_leak_scope(&env,true)==-EINVAL);
 assert(release_reference_state(&sub,parent)==-EINVAL);
 assert(release_reference_state(&sub,a)==0); /* release callback-owned ref */
 int b=acquire_reference_state(&env,3);
 assert(sub.refs[sub.acquired_refs-1].callback_ref==1);
 inner=sub; inner.frameno=3; inner.callback_frameno=3;
 inner.in_callback_fn=true; env.state=&inner;
 assert(release_reference_state(&inner,b)==-EINVAL);
 assert(check_reference_leak(&env)==0);
 int c=acquire_reference_state(&env,4);
 assert(check_reference_leak(&env)==-EINVAL);
 assert(release_reference_state(&inner,c)==0);
 assert(check_reference_leak(&env)==0);
 env.state=&sub; assert(release_reference_state(&sub,b)==0);
 outer.acquired_refs=sub.acquired_refs;
 memcpy(outer.refs,sub.refs,sizeof(sub.refs));
 env.state=&outer; assert(check_reference_leak(&env)==0);
 env.state=&main; assert(check_reference_leak(&env)==-EINVAL);
 assert(check_reference_leak_scope(&env,true)==-EINVAL);
 assert(release_reference_state(&main,parent)==0);
 assert(check_reference_leak(&env)==0);
 puts("PASS: callback/subprogram/nested reference ownership and tail-call leak checks");
}
'''
text = preamble + '\n'.join(function(n) for n in (
    'acquire_reference_state', 'release_reference_state',
    'check_reference_leak_scope', 'check_reference_leak')) + tests
with tempfile.TemporaryDirectory(prefix='bpf-callback-scope-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(text)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-Wall', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
