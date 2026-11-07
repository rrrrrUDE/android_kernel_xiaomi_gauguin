/*
 *  Burst-Oriented Response Enhancer (BORE) CPU Scheduler
 *  Copyright (C) 2021-2025 Masahito Suzuki <firelzrd@gmail.com>
 */
#include <linux/cpuset.h>
#include <linux/init.h>
#include <linux/sysctl.h>
#include <linux/sched/task.h>
#include <linux/sched/bore.h>
#include "sched.h"

#ifdef CONFIG_SCHED_BORE
DEFINE_STATIC_KEY_TRUE(sched_bore_key);
unsigned int __read_mostly sched_bore                   = 1;
unsigned int __read_mostly sched_burst_inherit_type     = 2;
unsigned int __read_mostly sched_burst_protect_slice_lv = 1;
unsigned int __read_mostly sched_burst_smoothness       = 1;
unsigned int __read_mostly sched_burst_penalty_offset   = 24;
uint __read_mostly sched_burst_penalty_scale    = 1536;
uint __read_mostly sched_burst_cache_lifetime   = 75000000;

uint __read_mostly sched_credit_cap_us = 16000;

static int __maybe_unused maxval_prio    =   39;
static int __maybe_unused maxval_6_bits  =   63;
static int __maybe_unused maxval_8_bits  =  255;
static int __maybe_unused maxval_12_bits = 4095;
static int __maybe_unused maxval_16_bits = 65535;
static int __maybe_unused maxval_1_million = 1000000;

#define MAX_BURST_PENALTY ((40U << 8) - 1)
#define BURST_CACHE_SAMPLE_LIMIT 63
#define BURST_CACHE_SCAN_LIMIT (BURST_CACHE_SAMPLE_LIMIT * 2)

static u32 bore_reciprocal_lut[BURST_CACHE_SAMPLE_LIMIT + 1];

DEFINE_STATIC_KEY_TRUE(sched_burst_inherit_key);
DEFINE_STATIC_KEY_TRUE(sched_burst_ancestor_key);
DEFINE_STATIC_KEY_TRUE(sched_credit_key);

static inline u32 log2p1_u64_u32fp(u64 v, u8 fp)
{
	int clz, exponent;
	u32 mantissa;

	if (unlikely(!v))
		return 0;
	clz = __builtin_clzll(v);
	exponent = 64 - clz;
	mantissa = (u32)((v << clz) << 1 >> (64 - fp));
	return exponent << fp | mantissa;
}

void bore_note_sleep(struct task_struct *p, u64 now)
{
	p->bore.credit_sleep = now;
}

u64 bore_credit_ns(struct task_struct *p)
{
	u64 cap = (u64)READ_ONCE(sched_credit_cap_us) * 1000ULL;
	u64 slept = 0, credit = 0;

	if (p->bore.credit_sleep) {
		slept = ktime_get_ns() - p->bore.credit_sleep;
		if ((s64)slept > 0) {
			p->bore.credit_sleep = 0;
			credit = slept < cap ? slept : cap;
		} else {
			slept = 0;
		}
	}

	return credit;
}

static inline u32 calc_burst_penalty(u64 burst_time) {
	u32 greed = log2p1_u64_u32fp(burst_time, 8),
		tolerance = READ_ONCE(sched_burst_penalty_offset) << 8;
	s32 diff = (s32)(greed - tolerance);
	u32 penalty = diff & ~(diff >> 31);
	u32 scaled_penalty = penalty * READ_ONCE(sched_burst_penalty_scale) >> 10;
	s32 overflow = scaled_penalty - MAX_BURST_PENALTY;
	return scaled_penalty - (overflow & ~(overflow >> 31));
}

static inline u32 binary_smooth(u32 new, u32 old) {
	u32 is_growing = (new > old);
	u32 increment = (new - old) * is_growing;
	u32 shift = READ_ONCE(sched_burst_smoothness);
	u32 smoothed = old + ((increment + (1U << shift) - 1) >> shift);
	return (new & ~(-is_growing)) | (smoothed & (-is_growing));
}

static void reweight_task_by_prio(struct task_struct *p, int prio) {
	if (task_has_idle_policy(p)) return;

	/* 4.19 reweight_task maintains PELT and runnable_weight, also off-rq. */
	p->bore.stop_update = true;
	reweight_task(p, prio);
	p->bore.stop_update = false;
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

static void update_penalty(struct task_struct *p) {
	struct bore_ctx *ctx = &p->bore;

	u8 new_prio, prev_prio = effective_prio_bore(p);

	s32 diff = (s32)ctx->curr_penalty - (s32)ctx->prev_penalty;
	u16 max_val = ctx->curr_penalty - (diff & (diff >> 31));
	u32 is_kthread = !!(p->flags & PF_KTHREAD);

	WRITE_ONCE(ctx->penalty, max_val & -(s32)(!is_kthread));

	new_prio = effective_prio_bore(p);
	if (new_prio != prev_prio)
		reweight_task_by_prio(p, new_prio);
}

void update_curr_bore(struct task_struct *p, u64 delta_exec) {
	struct bore_ctx *ctx = &p->bore;
	u32 curr_penalty;

	if (ctx->stop_update) return;

	ctx->burst_time = min_t(u64, ctx->burst_time, U64_MAX - delta_exec) + delta_exec;
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

static inline bool task_is_bore_eligible(struct task_struct *p)
{return p && READ_ONCE(p->sched_class) == &fair_sched_class &&
	!READ_ONCE(p->exit_state) && !(READ_ONCE(p->flags) & PF_EXITING);}

#ifndef for_each_child_task
#define for_each_child_task(p, t) \
	list_for_each_entry(t, &(p)->children, sibling)
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
	return now - timestamp > (u64)READ_ONCE(sched_burst_cache_lifetime);
}

static void update_burst_cache(struct bore_bc *bc,
		struct task_struct *p, u32 count, u32 total, u64 now) {
	u32 average = (count == 1) ? total :
		(u32)(((u64)total * bore_reciprocal_lut[count]) >> 32);

	struct bore_bc new_bc = {
		.penalty = max(average, (u32)READ_ONCE(p->bore.penalty)),
		.timestamp = now >> BORE_BC_TIMESTAMP_SHIFT
	};
	WRITE_ONCE(bc->value, new_bc.value);
}

static u32 inherit_from_parent(struct task_struct *parent,
									u64 clone_flags, u64 now) {
	struct bore_bc bc_val;
	struct bore_bc *bc;

	if (clone_flags & CLONE_PARENT)
		parent = parent->real_parent;

	bc = &parent->bore.subtree;

	if (burst_cache_expired(bc, now)) {
		struct task_struct *child;
		u32 count = 0, total = 0, scan_count = 0;
		for_each_child_task(parent, child) {
			if (count >= BURST_CACHE_SAMPLE_LIMIT) break;
			if (scan_count++ >= BURST_CACHE_SCAN_LIMIT) break;

			if (!task_is_bore_eligible(child)) continue;
			count++;
			total += READ_ONCE(child->bore.penalty);
		}

		update_burst_cache(bc, parent, count, total, now);
	}

	bc_val.value = READ_ONCE(bc->value);
	return (u32)bc_val.penalty;
}

static u32 inherit_from_ancestor_hub(struct task_struct *parent,
										u64 clone_flags, u64 now) {
	struct bore_bc bc_val;
	struct task_struct *ancestor = parent, *next;
	struct bore_bc *bc;
	u32 sole_child_count = 0, depth = 0;

	if (clone_flags & CLONE_PARENT) {
		ancestor = ancestor->real_parent;
		sole_child_count = 1;
	}

	for (;
			(next = ancestor->real_parent) != ancestor &&
			count_children_upto2(ancestor) <= sole_child_count &&
			depth++ < BURST_CACHE_SCAN_LIMIT;
			ancestor = next, sole_child_count = 1) {}

	bc = &ancestor->bore.subtree;

	if (burst_cache_expired(bc, now)) {
		struct task_struct *direct_child;
		u32 count = 0, total = 0, scan_count = 0;
		for_each_child_task(ancestor, direct_child) {
			struct task_struct *descendant = direct_child;

			if (count >= BURST_CACHE_SAMPLE_LIMIT) break;
			if (scan_count++ >= BURST_CACHE_SCAN_LIMIT) break;

			while (scan_count++ < BURST_CACHE_SCAN_LIMIT &&
			       count_children_upto2(descendant) == 1) {
				struct task_struct *next_descendant =
					list_first_entry_or_null(&descendant->children,
											struct task_struct, sibling);
				if (!next_descendant) break;
				descendant = next_descendant;
			}

			if (!task_is_bore_eligible(descendant)) continue;
			count++;
			total += READ_ONCE(descendant->bore.penalty);
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

	if (burst_cache_expired(bc, now)) {
		struct task_struct *sibling;
		u32 count = 0, total = 0, scan_count = 0;

		for_each_thread(leader, sibling) {
			if (count >= BURST_CACHE_SAMPLE_LIMIT) break;
			if (scan_count++ >= BURST_CACHE_SCAN_LIMIT) break;

			if (!task_is_bore_eligible(sibling)) continue;
			count++;
			total += READ_ONCE(sibling->bore.penalty);
		}

		update_burst_cache(bc, leader, count, total, now);
	}

	bc_val.value = READ_ONCE(bc->value);
	return (u32)bc_val.penalty;
}

void task_fork_bore(struct task_struct *p,
	               struct task_struct *parent, u64 clone_flags, u64 now) {
	unsigned long flags;
	struct bore_ctx *ctx = &p->bore;
	u32 inherited_penalty;
	u16 previous = READ_ONCE(parent->bore.prev_penalty);
	reset_task_bore(p);
	if (!task_is_bore_eligible(p))
		return;
	if (!static_branch_likely(&sched_bore_key)) {
		unsigned int prio = effective_prio_bore(p);
		if (!task_has_idle_policy(p)) {
			p->se.load.weight = scale_load(sched_prio_to_weight[prio]);
			p->se.runnable_weight = p->se.load.weight;
			p->se.load.inv_weight = sched_prio_to_wmult[prio];
		}
		return;
	}
	p->bore.prev_penalty = previous;

	read_lock_irqsave(&tasklist_lock, flags);
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
	ctx->penalty = (p->flags & PF_KTHREAD) ? 0 : ctx->prev_penalty;
	if (!task_has_idle_policy(p)) {
		u8 prio = effective_prio_bore(p);
		p->se.load.weight = scale_load(sched_prio_to_weight[prio]);
		p->se.runnable_weight = p->se.load.weight;
		p->se.load.inv_weight = sched_prio_to_wmult[prio];
	}
	read_unlock_irqrestore(&tasklist_lock, flags);
}

void reset_task_bore(struct task_struct *p)
{ memset(&p->bore, 0, sizeof(struct bore_ctx)); }

void task_dead_bore(struct task_struct *p)
{
	/* No reweighting after the final context switch. */
	WRITE_ONCE(p->bore.penalty, 0);
	p->bore.burst_time = 0;
	p->bore.credit_sleep = 0;
	p->bore.prev_penalty = 0;
	p->bore.curr_penalty = 0;
	p->bore.futex_waiting = false;
	p->bore.stop_update = true;
	WRITE_ONCE(p->bore.subtree.value, 0);
	WRITE_ONCE(p->bore.group.value, 0);
}

void __init sched_init_bore(void) {
	int i;
	printk(KERN_INFO "%s %s by %s\n",
		SCHED_BORE_PROGNAME, SCHED_BORE_VERSION, SCHED_BORE_AUTHOR);

	for (i = 1; i <= BURST_CACHE_SAMPLE_LIMIT; i++)
		bore_reciprocal_lut[i] = (u32)div64_u64(0xffffffffULL + i, i);

	reset_task_bore(&init_task);

}

#ifdef CONFIG_SYSCTL
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

static void readjust_all_task_weights(void) {
	struct task_struct *task, *group;
	struct rq *rq;
	struct rq_flags rf;

	rcu_read_lock();
	for_each_process_thread(group, task) {
		if (!task_is_bore_eligible(task)) continue;
		rq = task_rq_lock(task, &rf);
		update_rq_clock(rq);
		if (task_is_bore_eligible(task))
			reweight_task_by_prio(task, effective_prio_bore(task));
		task_rq_unlock(rq, task, &rf);
	}
	rcu_read_unlock();
}

static DEFINE_MUTEX(bore_sysctl_mutex);

int sched_bore_update_handler(struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos) {
	int ret;

	mutex_lock(&bore_sysctl_mutex);
	ret = proc_douintvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		goto out;

	if (sched_bore)
		static_branch_enable(&sched_bore_key);
	else
		static_branch_disable(&sched_bore_key);

	readjust_all_task_weights();

out:
	mutex_unlock(&bore_sysctl_mutex);
	return ret;
}

int sched_burst_inherit_type_update_handler(struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos) {
	int ret;

	mutex_lock(&bore_sysctl_mutex);
	ret = proc_douintvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		goto out;

	update_inherit_type();

out:
	mutex_unlock(&bore_sysctl_mutex);
	return ret;
}

int sched_burst_protect_slice_lv_update_handler(struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos) {
	int ret;

	mutex_lock(&bore_sysctl_mutex);
	ret = proc_douintvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		goto out;

out:
	mutex_unlock(&bore_sysctl_mutex);
	return ret;
}

int sched_credit_cap_us_update_handler(struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp, loff_t *ppos) {
	int ret;

	mutex_lock(&bore_sysctl_mutex);
	ret = proc_douintvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		goto out;

	if (sched_credit_cap_us)
		static_branch_enable(&sched_credit_key);
	else
		static_branch_disable(&sched_credit_key);

out:
	mutex_unlock(&bore_sysctl_mutex);
	return ret;
}

static unsigned int bore_zero, bore_one = 1, bore_two = 2, bore_three = 3;
static unsigned int bore_slice_min = 1, bore_slice_max = 1000000000;
extern void sched_update_min_base_slice(void);
static int bore_slice_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_douintvec_minmax(table, write, buffer, lenp, ppos);

	if (!ret && write)
		sched_update_min_base_slice();
	return ret;
}

static char sched_bore_version[] = SCHED_BORE_VERSION;
static struct ctl_table sched_bore_sysctls[] = {
	{
		.procname = "sched_bore_version",
		.data = sched_bore_version,
		.maxlen = sizeof(sched_bore_version),
		.mode = 0444,
		.proc_handler = proc_dostring,
	},
	{
		.procname = "sched_min_base_slice_ns",
		.data = &sysctl_sched_min_base_slice,
		.maxlen = sizeof(unsigned int),
		.mode = 0644,
		.proc_handler = bore_slice_handler,
		.extra1 = &bore_slice_min,
		.extra2 = &bore_slice_max,
	},
	{
		.procname	= "sched_bore",
		.data		= &sched_bore,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler = sched_bore_update_handler,
		.extra1		= &bore_zero,
		.extra2		= &bore_one,
	},
	{
		.procname	= "sched_burst_inherit_type",
		.data		= &sched_burst_inherit_type,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler = sched_burst_inherit_type_update_handler,
		.extra1		= &bore_zero,
		.extra2		= &bore_two,
	},
	{
		.procname	= "sched_burst_smoothness",
		.data		= &sched_burst_smoothness,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &bore_three,
	},
	{
		.procname	= "sched_burst_penalty_offset",
		.data		= &sched_burst_penalty_offset,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &maxval_6_bits,
	},
	{
		.procname	= "sched_burst_penalty_scale",
		.data		= &sched_burst_penalty_scale,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &maxval_12_bits,
	},
	{
		.procname	= "sched_burst_cache_lifetime",
		.data		= &sched_burst_cache_lifetime,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler = proc_douintvec,
	},
	{
		.procname	= "sched_burst_protect_slice_lv",
		.data		= &sched_burst_protect_slice_lv,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler = sched_burst_protect_slice_lv_update_handler,
		.extra1		= &bore_zero,
		.extra2		= &bore_three,
	},
	{
		.procname	= "sched_credit_cap_us",
		.data		= &sched_credit_cap_us,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler = sched_credit_cap_us_update_handler,
		.extra1		= &bore_zero,
		.extra2		= &maxval_1_million,
	},
	{ }
};

static int __init sched_bore_sysctl_init(void) {
	if (!register_sysctl("kernel", sched_bore_sysctls))
		return -ENOMEM;
	return 0;
}
late_initcall(sched_bore_sysctl_init);

#endif // CONFIG_SYSCTL
#endif /* CONFIG_SCHED_BORE */
