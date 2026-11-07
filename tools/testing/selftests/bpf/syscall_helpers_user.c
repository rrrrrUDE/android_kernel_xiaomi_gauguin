// SPDX-License-Identifier: GPL-2.0
/* Standalone: build against this kernel's exported UAPI headers.
 * Run as root on the patched kernel. No libbpf dependency.
 */
#define _GNU_SOURCE
#include <linux/bpf.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define I(code_, dst_, src_, off_, imm_) \
 ((struct bpf_insn){.code=(code_),.dst_reg=(dst_),.src_reg=(src_),.off=(off_),.imm=(imm_)})
#define MOV(d,s) I(BPF_ALU64|BPF_MOV|BPF_X,d,s,0,0)
#define IMM(d,v) I(BPF_ALU64|BPF_MOV|BPF_K,d,0,0,v)
#define ADD(d,v) I(BPF_ALU64|BPF_ADD|BPF_K,d,0,0,v)
#define CALL(id) I(BPF_JMP|BPF_CALL,0,0,0,BPF_FUNC_##id)
#define STX(d,s,o) I(BPF_STX|BPF_MEM|BPF_DW,d,s,o,0)
#define LDX(d,s,o) I(BPF_LDX|BPF_MEM|BPF_DW,d,s,o,0)
struct context {
 uint64_t user_ptr, cpu_before, cpu_after;
 int64_t copy_ret, copied, fd, close_ret;
};
static char log_buf[262144];
static int sys_bpf(int cmd, union bpf_attr *a)
{
 return syscall(__NR_bpf,cmd,a,sizeof(*a));
}
int main(int argc, char **argv)
{
 unsigned int loops=argc>1 ? strtoul(argv[1],NULL,0):1000, i;
 struct bpf_insn insn[]={
  MOV(6,1), CALL(get_smp_processor_id), STX(6,0,8),
  MOV(1,10), ADD(1,-8), IMM(2,8), LDX(3,6,0), CALL(copy_from_user),
  STX(6,0,24), LDX(7,10,-8), STX(6,7,32),
  CALL(get_smp_processor_id), STX(6,0,16),
  /* Create an array map using an initialized on-stack bpf_attr prefix. */
  I(BPF_ST|BPF_MEM|BPF_DW,10,0,-40,0),
  I(BPF_ST|BPF_MEM|BPF_DW,10,0,-32,0),
  I(BPF_ST|BPF_MEM|BPF_DW,10,0,-24,0),
  I(BPF_ST|BPF_MEM|BPF_W,10,0,-40,BPF_MAP_TYPE_ARRAY),
  I(BPF_ST|BPF_MEM|BPF_W,10,0,-36,4),
  I(BPF_ST|BPF_MEM|BPF_W,10,0,-32,8),
  I(BPF_ST|BPF_MEM|BPF_W,10,0,-28,1),
  IMM(1,BPF_MAP_CREATE), MOV(2,10), ADD(2,-40), IMM(3,24), CALL(sys_bpf),
  STX(6,0,40), MOV(1,0), CALL(sys_close), STX(6,0,48),
  IMM(0,0), I(BPF_JMP|BPF_EXIT,0,0,0,0),
 };
 union bpf_attr a={};
 int fd;
 long page_size=sysconf(_SC_PAGESIZE);
 void *page=mmap(NULL,page_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(page==MAP_FAILED) { perror("mmap"); return 1; }
 a.prog_type=BPF_PROG_TYPE_SYSCALL;
 a.prog_flags=BPF_F_SLEEPABLE;
 a.insns=(uintptr_t)insn;
 a.insn_cnt=sizeof(insn)/sizeof(insn[0]);
 a.license=(uintptr_t)"GPL";
 a.log_buf=(uintptr_t)log_buf; a.log_size=sizeof(log_buf); a.log_level=1;
 fd=sys_bpf(BPF_PROG_LOAD,&a);
 if(fd<0) { perror("BPF_PROG_LOAD"); fputs(log_buf,stderr); return 1; }
 for(i=0;i<loops;i++) {
  struct context ctx={.user_ptr=(uintptr_t)page};
  /* Drop the user PTE: copy_from_user must be able to fault and sleep. */
  if(madvise(page,page_size,MADV_DONTNEED)) { perror("madvise"); return 1; }
  memset(&a,0,sizeof(a)); a.test.prog_fd=fd;
  a.test.ctx_in=(uintptr_t)&ctx; a.test.ctx_size_in=sizeof(ctx);
  if(sys_bpf(BPF_PROG_TEST_RUN,&a)) { perror("BPF_PROG_TEST_RUN"); return 1; }
  if(ctx.copy_ret || ctx.copied || ctx.cpu_before!=ctx.cpu_after ||
     ctx.fd<0 || ctx.close_ret) {
   fprintf(stderr,"FAIL loop=%u cpu=%llu/%llu copy=%lld data=%lld fd=%lld close=%lld\n",
    i,(unsigned long long)ctx.cpu_before,(unsigned long long)ctx.cpu_after,
    (long long)ctx.copy_ret,(long long)ctx.copied,(long long)ctx.fd,(long long)ctx.close_ret);
   return 1;
  }
 }
 close(fd); munmap(page,page_size);
 printf("PASS: %u SYSCALL runs with user page faults, CPU locality, sys_bpf and sys_close\n",loops);
 return 0;
}
