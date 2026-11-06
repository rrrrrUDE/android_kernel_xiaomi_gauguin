#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile and execute production helper-argument normalization.
Does not substitute for verifier load tests on the target kernel.
"""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
header = (root / 'include/linux/bpf.h').read_text()
start = header.index('/* Base type and orthogonal')
end = header.index('/* eBPF function prototype used by verifier', start)
source = '#include <assert.h>\n#define BIT(x) (1U << (x))\n' + header[start:end]
source += r'''
int main(void)
{
 int bases[] = { ARG_PTR_TO_MAP_VALUE, ARG_PTR_TO_MEM, ARG_PTR_TO_CTX,
                 ARG_PTR_TO_SOCKET, ARG_PTR_TO_ALLOC_MEM, ARG_PTR_TO_STACK };
 int nullable[] = { ARG_PTR_TO_MAP_VALUE_OR_NULL, ARG_PTR_TO_MEM_OR_NULL,
                    ARG_PTR_TO_CTX_OR_NULL, ARG_PTR_TO_SOCKET_OR_NULL,
                    ARG_PTR_TO_ALLOC_MEM_OR_NULL, ARG_PTR_TO_STACK_OR_NULL };
 unsigned int i;
 for (i=0;i<sizeof(bases)/sizeof(*bases);i++) {
  assert(bpf_arg_type_no_readonly(bases[i]) == bases[i]);
  assert(bpf_arg_type_no_readonly(bases[i] | PTR_MAYBE_NULL) == nullable[i]);
  assert(bpf_arg_type_no_readonly(bases[i] | PTR_MAYBE_NULL | MEM_RDONLY) == nullable[i]);
  assert(bpf_arg_type_no_readonly(nullable[i] | MEM_RDONLY) == nullable[i]);
 }
 assert(base_type(ARG_PTR_TO_MEM | MEM_RDONLY | PTR_MAYBE_NULL) == ARG_PTR_TO_MEM);
 assert((RET_PTR_TO_MAP_VALUE | PTR_MAYBE_NULL) == RET_PTR_TO_MAP_VALUE_OR_NULL);
 assert((RET_PTR_TO_BTF_ID | PTR_MAYBE_NULL) == RET_PTR_TO_BTF_ID_OR_NULL);
 assert((RET_PTR_TO_MEM_OR_BTF_ID | PTR_MAYBE_NULL) == RET_PTR_TO_MEM_OR_BTF_ID_OR_NULL);
 assert(__BPF_ARG_TYPE_MAX < (1U << BPF_BASE_TYPE_BITS));
 assert(RET_PTR_TO_SOCKET_OR_NULL == (RET_PTR_TO_SOCKET | PTR_MAYBE_NULL));
 assert(RET_PTR_TO_ALLOC_MEM_OR_NULL == (RET_PTR_TO_ALLOC_MEM | PTR_MAYBE_NULL));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bpf-modifiers-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(source)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-Wall', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: nullable flags and read-only argument normalization')
