/*
 *  Burst-Oriented Response Enhancer (BORE) CPU Scheduler
 *  Copyright (C) 2021-2025 Masahito Suzuki <firelzrd@gmail.com>
 *  Copyright (C) 2026 Salman Wahib <sxlmnwb.dev@gmail.com>
 */
#include <linux/cpuset.h>
#include <linux/ktime.h>
#include <linux/sysctl.h>
#include <linux/sched/task.h>
#include <linux/sched/bore.h>
#include "sched.h"

#ifdef CONFIG_SCHED_BORE
DEFINE_STATIC_KEY_TRUE(sched_bore_key);
unsigned int __read_mostly sched_bore                   = 1;
unsigned int __read_mostly sched_burst_inherit_type     = 2;
unsigned int __read_mostly sched_burst_smoothness       = 1;
unsigned int __read_mostly sched_burst_penalty_offset   = 24;
unsigned int __read_mostly sched_burst_penalty_scale    = 1536;
unsigned int __read_mostly sched_burst_cache_lifetime   = 75000000;
unsigned int __read_mostly sched_credit_cap_us          = 16000;
static int maxval_prio    =   39;

#define MAX_BURST_PENALTY ((40U << 8) - 1)
#define BURST_CACHE_SAMPLE_LIMIT 63
#define BURST_CACHE_SCAN_LIMIT (BURST_CACHE_SAMPLE_LIMIT * 2)

static u32 bore_reciprocal_lut[BURST_CACHE_SAMPLE_LIMIT + 1];

DEFINE_STATIC_KEY_TRUE(sched_burst_inherit_key);
DEFINE_STATIC_KEY_TRUE(sched_burst_ancestor_key);
DEFINE_STATIC_KEY_FALSE(sched_credit_key);

static inline u32 log2p1_u64_u32fp(u64 v, u8 fp) {
    int clz;
    int exponent;
    u32 mantissa;

    if (unlikely(!v)) return 0;
    clz = __builtin_clzll(v);
    exponent = 64 - clz;
    mantissa = (u32)((v << clz) << 1 >> (64 - fp));
    return exponent << fp | mantissa;
}

/*
 * Record when a task starts sleeping.  On the next wakeup, a bounded part of
 * that sleep time may be used as a one-shot vruntime credit. Linux 4.19 uses
 * classic CFS vruntime rather than EEVDF deadlines, so the credit is applied
 * at entity placement instead of adjusting se->deadline.
 */
void bore_note_sleep(struct task_struct *p, u64 now)
{
    WRITE_ONCE(p->bore.credit_sleep, now);
}

u64 bore_credit_ns(struct task_struct *p)
{
    u64 cap = (u64)sched_credit_cap_us * 1000ULL;
    u64 slept;

    if (!p->bore.credit_sleep)
        return 0;

    slept = ktime_get_ns();
    if (slept <= p->bore.credit_sleep) {
        p->bore.credit_sleep = 0;
        return 0;
    }

    slept -= p->bore.credit_sleep;
    p->bore.credit_sleep = 0;
    return min_t(u64, slept, cap);
}

static inline u32 calc_burst_penalty(u64 burst_time) {
    u32 greed, tolerance, penalty, scaled_penalty;
    s32 diff, overflow;

    greed = log2p1_u64_u32fp(burst_time, 8);
    tolerance = sched_burst_penalty_offset << 8;
    diff = (s32)(greed - tolerance);
    penalty = diff & ~(diff >> 31);
    scaled_penalty = penalty * sched_burst_penalty_scale >> 10;
    overflow = scaled_penalty - MAX_BURST_PENALTY;
    return scaled_penalty - (overflow & ~(overflow >> 31));
}

static inline u32 binary_smooth(u32 new, u32 old) {
    u32 is_growing = (new > old);
    u32 increment = (new - old) * is_growing;
    u32 shift = sched_burst_smoothness;
    u32 smoothed = old + ((increment + (1U << shift) - 1) >> shift);
    return (new & ~(-is_growing)) | (smoothed & (-is_growing));
}

u8 effective_prio_bore(struct task_struct *p) {
    int prio = p->static_prio - MAX_RT_PRIO;
    s32 diff;

    if (static_branch_likely(&sched_bore_key))
        prio += bore_score(p);
    prio &= ~(prio >> 31);
    diff = prio - maxval_prio;
    prio -= (diff & ~(diff >> 31));
    return (u8)prio;
}

static void reweight_task_bore(struct task_struct *p, int prio) {
    struct sched_entity *se = &p->se;
    unsigned long weight;

    if (p->policy == SCHED_IDLE) return;

    weight = scale_load(sched_prio_to_weight[prio]);

    if (se->on_rq) {
        p->bore.stop_update = true;
        bore_reweight_entity(p, weight);
        p->bore.stop_update = false;
    } else {
        se->load.weight = weight;
        se->runnable_weight = weight;
    }
    se->load.inv_weight = sched_prio_to_wmult[prio];
}

static void update_penalty(struct task_struct *p) {
    struct bore_ctx *ctx = &p->bore;
    u8  prev_prio = effective_prio_bore(p);
    s32 diff;
    u16 max_val;
    u32 is_kthread;
    u8 new_prio;

    diff = (s32)ctx->curr_penalty - (s32)ctx->prev_penalty;
    max_val = ctx->curr_penalty - (diff & (diff >> 31));
    is_kthread = !!(p->flags & PF_KTHREAD);
    ctx->penalty = max_val & -(s32)(!is_kthread);

    new_prio = effective_prio_bore(p);
    if (new_prio != prev_prio)
        reweight_task_bore(p, new_prio);
}

void update_curr_bore(struct task_struct *p, u64 delta_exec) {
    struct bore_ctx *ctx = &p->bore;
    u32 curr_penalty;

    if (ctx->stop_update) return;

    ctx->burst_time += delta_exec;
    curr_penalty = ctx->curr_penalty = calc_burst_penalty(ctx->burst_time);

    if (curr_penalty <= ctx->prev_penalty) return;
    update_penalty(p);
}

void restart_burst_bore(struct task_struct *p) {
    struct bore_ctx *ctx = &p->bore;
    u32 new_penalty = binary_smooth(ctx->curr_penalty, ctx->prev_penalty);
    ctx->prev_penalty = new_penalty;
    ctx->curr_penalty = 0;
    ctx->burst_time = 0;
    update_penalty(p);
}

/* Linux 4.19 has no EEVDF virtual deadline to rescale. */
void restart_burst_rescale_deadline_bore(struct task_struct *p)
{
    restart_burst_bore(p);
}

static inline bool task_is_bore_eligible(struct task_struct *p)
{return p && p->sched_class == &fair_sched_class && !p->exit_state;}

#ifndef for_each_child_task
#define for_each_child_task(p, t) \
    list_for_each_entry_rcu(t, &(p)->children, sibling)
#endif

static inline u32 count_children_upto2(struct task_struct *p) {
    struct list_head *head = &p->children;
    struct list_head *first = READ_ONCE(head->next);
    struct list_head *second = READ_ONCE(first->next);
    return (first != head) + (second != head);
}

static inline bool burst_cache_expired(struct bore_bc *bc, u64 now) {
    struct bore_bc bc_val = { .value = READ_ONCE(bc->value) };
    u64 timestamp = (u64)bc_val.timestamp << BORE_BC_TIMESTAMP_SHIFT;
    return now - timestamp > (u64)sched_burst_cache_lifetime;
}

static void update_burst_cache(struct bore_bc *bc,
        struct task_struct *p, u32 count, u32 total, u64 now) {
    u32 average;
    struct bore_bc new_bc;

    average = (count == 1) ? total :
        (u32)(((u64)total * bore_reciprocal_lut[count]) >> 32);

    new_bc.penalty = max_t(u32, average, p->bore.penalty);
    new_bc.timestamp = now >> BORE_BC_TIMESTAMP_SHIFT;
    WRITE_ONCE(bc->value, new_bc.value);
}

static u32 inherit_from_parent(struct task_struct *parent,
                                    u64 clone_flags, u64 now) {
    struct bore_bc bc_val;
    struct bore_bc *bc;
    struct task_struct *child;
    u32 count = 0, total = 0, scan_count = 0;

    if (clone_flags & CLONE_PARENT)
        parent = rcu_dereference(parent->real_parent);

    bc = &parent->bore.subtree;

    if (burst_cache_expired(bc, now)) {
        for_each_child_task(parent, child) {
            if (count >= BURST_CACHE_SAMPLE_LIMIT) break;
            if (scan_count++ >= BURST_CACHE_SCAN_LIMIT) break;

            if (!task_is_bore_eligible(child)) continue;
            count++;
            total += child->bore.penalty;
        }

        update_burst_cache(bc, parent, count, total, now);
    }

    bc_val.value = READ_ONCE(bc->value);
    return (u32)bc_val.penalty;
}

static u32 inherit_from_ancestor_hub(struct task_struct *parent,
                                        u64 clone_flags, u64 now) {
    struct bore_bc bc_val;
    struct task_struct *ancestor = parent;
    struct task_struct *next;
    struct bore_bc *bc;
    struct task_struct *direct_child;
    u32 sole_child_count = 0;
    u32 count = 0, total = 0, scan_count = 0;

    if (clone_flags & CLONE_PARENT) {
        ancestor = rcu_dereference(ancestor->real_parent);
        sole_child_count = 1;
    }

    for (;
            (next = rcu_dereference(ancestor->real_parent)) != ancestor &&
            count_children_upto2(ancestor) <= sole_child_count;
            ancestor = next, sole_child_count = 1) {}

    bc = &ancestor->bore.subtree;

    if (burst_cache_expired(bc, now)) {
        for_each_child_task(ancestor, direct_child) {
            struct task_struct *descendant = direct_child;
            struct task_struct *next_descendant;

            if (count >= BURST_CACHE_SAMPLE_LIMIT) break;
            if (scan_count++ >= BURST_CACHE_SCAN_LIMIT) break;

            while (count_children_upto2(descendant) == 1) {
                next_descendant =
                    list_first_or_null_rcu(&descendant->children,
                                            struct task_struct, sibling);
                if (!next_descendant) break;
                descendant = next_descendant;
            }

            if (!task_is_bore_eligible(descendant)) continue;
            count++;
            total += descendant->bore.penalty;
        }

        update_burst_cache(bc, ancestor, count, total, now);
    }

    bc_val.value = READ_ONCE(bc->value);
    return (u32)bc_val.penalty;
}

static u32 inherit_from_thread_group(struct task_struct *p, u64 now) {
    struct bore_bc bc_val;
    struct task_struct *leader = p->group_leader;
    struct bore_bc *bc = &leader->bore.group;
    struct task_struct *sibling;
    u32 count = 0, total = 0, scan_count = 0;

    if (burst_cache_expired(bc, now)) {
        for_each_thread(leader, sibling) {
            if (count >= BURST_CACHE_SAMPLE_LIMIT) break;
            if (scan_count++ >= BURST_CACHE_SCAN_LIMIT) break;

            if (!task_is_bore_eligible(sibling)) continue;
            count++;
            total += sibling->bore.penalty;
        }

        update_burst_cache(bc, leader, count, total, now);
    }

    bc_val.value = READ_ONCE(bc->value);
    return (u32)bc_val.penalty;
}

void task_fork_bore(struct task_struct *p,
                   struct task_struct *parent, u64 clone_flags, u64 now) {
    struct bore_ctx *ctx = &p->bore;
    u32 inherited_penalty;

    if (!static_branch_likely(&sched_bore_key) || !task_is_bore_eligible(p)) return;

    rcu_read_lock();
    if (clone_flags & CLONE_THREAD)
        inherited_penalty = inherit_from_thread_group(parent, now);
    else if (static_branch_likely(&sched_burst_inherit_key))
        inherited_penalty = static_branch_likely(&sched_burst_ancestor_key)?
            inherit_from_ancestor_hub(parent, clone_flags, now):
            inherit_from_parent(parent, clone_flags, now);
    else
        inherited_penalty = 0;

    if (ctx->prev_penalty < inherited_penalty)
        ctx->prev_penalty = inherited_penalty;
    ctx->curr_penalty  = 0;
    ctx->burst_time    = 0;
    ctx->stop_update   = false;
    ctx->futex_waiting = false;
    update_penalty(p);
    rcu_read_unlock();
}

void reset_task_bore(struct task_struct *p)
{ memset(&p->bore, 0, sizeof(struct bore_ctx)); }

static void update_inherit_type(void) {
    switch(sched_burst_inherit_type) {
    case 1:
        static_branch_enable(&sched_burst_inherit_key);
        static_branch_disable(&sched_burst_ancestor_key);
        break;
    case 2:
        static_branch_enable(&sched_burst_inherit_key);
        static_branch_enable(&sched_burst_ancestor_key);
        break;
    default:
        static_branch_disable(&sched_burst_inherit_key);
        break;
    }
}

void __init sched_init_bore(void) {
    int i;
    printk(KERN_INFO "%s %s by %s\n",
        SCHED_BORE_PROGNAME, SCHED_BORE_VERSION, SCHED_BORE_AUTHOR);

    for (i = 1; i <= BURST_CACHE_SAMPLE_LIMIT; i++)
        bore_reciprocal_lut[i] = (u32)div64_u64(0xffffffffULL + i, i);

    reset_task_bore(&init_task);
    update_inherit_type();

    if (sched_credit_cap_us)
        static_branch_enable(&sched_credit_key);
}

static void readjust_all_task_weights(void) {
    struct task_struct *task, *leader;
    struct rq *rq;
    struct rq_flags rf;

    write_lock_irq(&tasklist_lock);
    for_each_process_thread(leader, task) {
        if (!task_is_bore_eligible(task)) continue;
        rq = task_rq_lock(task, &rf);
        update_rq_clock(rq);
        reweight_task_bore(task, effective_prio_bore(task));
        task_rq_unlock(rq, task, &rf);
    }
    write_unlock_irq(&tasklist_lock);
}

#ifdef CONFIG_PROC_SYSCTL
int sched_bore_update_handler(struct ctl_table *table,
        int write, void __user *buffer, size_t *lenp, loff_t *ppos) {
    int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
    if (ret || !write)
        return ret;

    if (sched_bore)
        static_branch_enable(&sched_bore_key);
    else
        static_branch_disable(&sched_bore_key);

    readjust_all_task_weights();

    return 0;
}

int sched_credit_cap_us_update_handler(struct ctl_table *table,
        int write, void __user *buffer, size_t *lenp, loff_t *ppos) {
    int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
    if (ret || !write)
        return ret;

    if (sched_credit_cap_us)
        static_branch_enable(&sched_credit_key);
    else
        static_branch_disable(&sched_credit_key);

    return 0;
}

int sched_burst_inherit_type_update_handler(struct ctl_table *table,
        int write, void __user *buffer, size_t *lenp, loff_t *ppos) {
    int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
    if (ret || !write)
        return ret;

    update_inherit_type();

    return 0;
}
#endif /* CONFIG_PROC_SYSCTL */

#endif /* CONFIG_SCHED_BORE */
