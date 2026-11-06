#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise production nested migration protection and switch mask handling.
Host stubs model scheduler locks; device stress remains necessary.
"""
from pathlib import Path
import subprocess
import tempfile
import os
root=Path(__file__).resolve().parents[4]
s=(root/'kernel/sched/core.c').read_text()
def function(signature):
 a=s.index(signature+'\n{')
 return s[a:s.index('\n}',a)+2]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#define likely(x) (x)
#define EXPORT_SYMBOL_GPL(x)
#define barrier() __asm__ volatile("" ::: "memory")
#define SCA_MIGRATE_DISABLE 2
#define SCA_MIGRATE_ENABLE 4
struct cpumask { unsigned int bits; };
struct set_affinity_pending;
struct task_struct {
 unsigned int migration_disabled;
 struct cpumask cpus_allowed;
 const struct cpumask *cpus_ptr;
};
struct rq { unsigned int cpu, nr_pinned; };
static struct task_struct task;
static struct task_struct *current=&task;
static struct rq runqueue;
static unsigned int preempt_count, enable_calls;
static const struct cpumask masks[]={{1},{2},{4},{8}};
#define this_rq() (&runqueue)
#define cpumask_of(cpu) (&masks[cpu])
#define is_migration_disabled(p) ((p)->migration_disabled)
static void preempt_disable(void) { preempt_count++; }
static void preempt_enable(void) { assert(preempt_count); preempt_count--; }
static void __do_set_cpus_allowed(struct task_struct *p, const struct cpumask *m, unsigned int flags)
{ assert(flags==SCA_MIGRATE_DISABLE); p->cpus_ptr=m; }
static int __set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *m, unsigned int flags)
{ assert(flags==SCA_MIGRATE_ENABLE); assert(preempt_count); p->cpus_ptr=m; enable_calls++; return 0; }
'''
for sig in ['static void migrate_disable_switch(struct rq *rq, struct task_struct *p)',
            'void migrate_disable(void)', 'void migrate_enable(void)']:
 code+=function(sig)+'\n'
code+=r'''
int main(void) {
 unsigned int cpu, depth, i;
 for(cpu=0;cpu<4;cpu++) for(depth=1;depth<=8;depth++) {
  task=(struct task_struct){.cpus_allowed={15}};
  task.cpus_ptr=&task.cpus_allowed;
  runqueue=(struct rq){.cpu=cpu};
  enable_calls=0;
  for(i=0;i<depth;i++) migrate_disable();
  assert(!preempt_count && task.migration_disabled==depth);
  assert(runqueue.nr_pinned==1);
  migrate_disable_switch(&runqueue,&task);
  assert(task.cpus_ptr==cpumask_of(cpu));
  /* An external affinity update changes requested, not effective mask. */
  task.cpus_allowed.bits=1U<<((cpu+1)%4);
  for(i=1;i<depth;i++) {
   migrate_enable();
   assert(task.cpus_ptr==cpumask_of(cpu) && runqueue.nr_pinned==1);
  }
  migrate_enable();
  assert(!preempt_count && !task.migration_disabled && !runqueue.nr_pinned);
  assert(task.cpus_ptr==&task.cpus_allowed && enable_calls==1);
 }
 puts("PASS: 32 production nested pin/switch/unpin cases with concurrent affinity updates");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='bpf-migration-') as d:
 p=Path(d); (p/'test.c').write_text(code)
 subprocess.run([os.environ.get('HOSTCC','cc'),'-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
