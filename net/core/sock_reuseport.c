// SPDX-License-Identifier: GPL-2.0
/*
 * To speed up listener socket lookup, create an array to store all sockets
 * listening on the same port.  This allows a decision to be made after finding
 * the first socket.  An optional BPF program can also be configured for
 * selecting the socket index from the array of available sockets.
 */

#include <net/sock_reuseport.h>
#include <net/ip.h>
#include <linux/bpf.h>
#include <linux/idr.h>
#include <linux/filter.h>
#include <linux/rcupdate.h>

#define INIT_SOCKS 128

DEFINE_SPINLOCK(reuseport_lock);

static DEFINE_IDA(reuseport_ida);
static void reuseport_free_rcu(struct rcu_head *head);
static int reuseport_resurrect(struct sock *sk, struct sock_reuseport *old_reuse,
			       struct sock_reuseport *reuse, bool bind_inany);

static int reuseport_sock_index(struct sock *sk,
				const struct sock_reuseport *reuse, bool closed)
{
	int i, end;

	if (closed) {
		i = reuse->max_socks - reuse->num_closed_socks;
		end = reuse->max_socks;
	} else {
		i = 0;
		end = reuse->num_socks;
	}
	for (; i < end; i++)
		if (reuse->socks[i] == sk)
			return i;
	return -1;
}

static void __reuseport_add_sock(struct sock *sk, struct sock_reuseport *reuse)
{
	reuse->socks[reuse->num_socks] = sk;
	smp_wmb();
	reuse->num_socks++;
}

static bool __reuseport_detach_sock(struct sock *sk,
				    struct sock_reuseport *reuse)
{
	int i = reuseport_sock_index(sk, reuse, false);

	if (i < 0)
		return false;
	reuse->socks[i] = reuse->socks[reuse->num_socks - 1];
	reuse->num_socks--;
	return true;
}

static void __reuseport_add_closed_sock(struct sock *sk,
					struct sock_reuseport *reuse)
{
	reuse->socks[reuse->max_socks - reuse->num_closed_socks - 1] = sk;
	WRITE_ONCE(reuse->num_closed_socks, reuse->num_closed_socks + 1);
}

static bool __reuseport_detach_closed_sock(struct sock *sk,
					   struct sock_reuseport *reuse)
{
	int i = reuseport_sock_index(sk, reuse, true);

	if (i < 0)
		return false;
	reuse->socks[i] = reuse->socks[reuse->max_socks - reuse->num_closed_socks];
	WRITE_ONCE(reuse->num_closed_socks, reuse->num_closed_socks - 1);
	return true;
}

static struct sock_reuseport *__reuseport_alloc(unsigned int max_socks)
{
	unsigned int size = sizeof(struct sock_reuseport) +
		      sizeof(struct sock *) * max_socks;
	struct sock_reuseport *reuse = kzalloc(size, GFP_ATOMIC);

	if (!reuse)
		return NULL;

	reuse->max_socks = max_socks;

	RCU_INIT_POINTER(reuse->prog, NULL);
	return reuse;
}

int reuseport_alloc(struct sock *sk, bool bind_inany)
{
	struct sock_reuseport *reuse;
	int id, ret = 0;

	/* bh lock used since this function call may precede hlist lock in
	 * soft irq of receive path or setsockopt from process context
	 */
	spin_lock_bh(&reuseport_lock);

	/* Allocation attempts can occur concurrently via the setsockopt path
	 * and the bind/hash path.  Nothing to do when we lose the race.
	 */
	reuse = rcu_dereference_protected(sk->sk_reuseport_cb,
					  lockdep_is_held(&reuseport_lock));
	if (reuse) {
		if (reuseport_sock_index(sk, reuse, true) >= 0) {
			ret = reuseport_resurrect(sk, reuse, NULL, bind_inany);
			goto out;
		}
		/* Only set reuse->bind_inany if the bind_inany is true.
		 * Otherwise, it will overwrite the reuse->bind_inany
		 * which was set by the bind/hash path.
		 */
		if (bind_inany)
			reuse->bind_inany = bind_inany;
		goto out;
	}

	reuse = __reuseport_alloc(INIT_SOCKS);
	if (!reuse) {
		ret = -ENOMEM;
		goto out;
	}

	id = ida_alloc(&reuseport_ida, GFP_ATOMIC);
	if (id < 0) {
		kfree(reuse);
		ret = id;
		goto out;
	}

	reuse->reuseport_id = id;
	reuse->socks[0] = sk;
	reuse->num_socks = 1;
	reuse->bind_inany = bind_inany;
	rcu_assign_pointer(sk->sk_reuseport_cb, reuse);

out:
	spin_unlock_bh(&reuseport_lock);

	return ret;
}
EXPORT_SYMBOL(reuseport_alloc);

static struct sock_reuseport *reuseport_grow(struct sock_reuseport *reuse)
{
	struct sock_reuseport *more_reuse;
	u32 more_socks_size, i;

	more_socks_size = reuse->max_socks * 2U;
	if (more_socks_size > U16_MAX) {
		if (reuse->num_closed_socks) {
			struct sock *sk;

			sk = reuse->socks[reuse->max_socks - reuse->num_closed_socks];
			RCU_INIT_POINTER(sk->sk_reuseport_cb, NULL);
			__reuseport_detach_closed_sock(sk, reuse);
			return reuse;
		}
		return NULL;
	}

	more_reuse = __reuseport_alloc(more_socks_size);
	if (!more_reuse)
		return NULL;

	more_reuse->max_socks = more_socks_size;
	more_reuse->num_socks = reuse->num_socks;
	more_reuse->num_closed_socks = reuse->num_closed_socks;
	more_reuse->prog = reuse->prog;
	more_reuse->reuseport_id = reuse->reuseport_id;
	more_reuse->bind_inany = reuse->bind_inany;
	more_reuse->has_conns = reuse->has_conns;

	memcpy(more_reuse->socks, reuse->socks,
	       reuse->num_socks * sizeof(struct sock *));
	memcpy(more_reuse->socks + more_reuse->max_socks -
	       more_reuse->num_closed_socks,
	       reuse->socks + reuse->max_socks - reuse->num_closed_socks,
	       reuse->num_closed_socks * sizeof(struct sock *));
	more_reuse->synq_overflow_ts = READ_ONCE(reuse->synq_overflow_ts);

	for (i = 0; i < reuse->num_socks; ++i)
		rcu_assign_pointer(reuse->socks[i]->sk_reuseport_cb,
				   more_reuse);
	for (i = reuse->max_socks - reuse->num_closed_socks;
	     i < reuse->max_socks; i++)
		rcu_assign_pointer(reuse->socks[i]->sk_reuseport_cb,
				   more_reuse);

	/* Note: we use kfree_rcu here instead of reuseport_free_rcu so
	 * that reuse and more_reuse can temporarily share a reference
	 * to prog.
	 */
	kfree_rcu(reuse, rcu);
	return more_reuse;
}

/* Reattach a shutdown listener when listen() is called again. */
static int reuseport_resurrect(struct sock *sk, struct sock_reuseport *old_reuse,
			       struct sock_reuseport *reuse, bool bind_inany)
{
	if (old_reuse == reuse) {
		__reuseport_detach_closed_sock(sk, old_reuse);
		__reuseport_add_sock(sk, old_reuse);
		return 0;
	}
	if (!reuse) {
		int id;

		reuse = __reuseport_alloc(INIT_SOCKS);
		if (!reuse)
			return -ENOMEM;
		id = ida_alloc(&reuseport_ida, GFP_ATOMIC);
		if (id < 0) {
			kfree(reuse);
			return id;
		}
		reuse->reuseport_id = id;
		reuse->bind_inany = bind_inany;
	} else if (reuse->num_socks + reuse->num_closed_socks ==
		   reuse->max_socks) {
		reuse = reuseport_grow(reuse);
		if (!reuse)
			return -ENOMEM;
	}
	__reuseport_detach_closed_sock(sk, old_reuse);
	__reuseport_add_sock(sk, reuse);
	rcu_assign_pointer(sk->sk_reuseport_cb, reuse);
	if (!old_reuse->num_socks && !old_reuse->num_closed_socks)
		call_rcu(&old_reuse->rcu, reuseport_free_rcu);
	return 0;
}

static void reuseport_free_rcu(struct rcu_head *head)
{
	struct sock_reuseport *reuse;

	reuse = container_of(head, struct sock_reuseport, rcu);
	sk_reuseport_prog_free(rcu_dereference_protected(reuse->prog, 1));
	ida_free(&reuseport_ida, reuse->reuseport_id);
	kfree(reuse);
}

/**
 *  reuseport_add_sock - Add a socket to the reuseport group of another.
 *  @sk:  New socket to add to the group.
 *  @sk2: Socket belonging to the existing reuseport group.
 *  May return ENOMEM and not add socket to group under memory pressure.
 */
int reuseport_add_sock(struct sock *sk, struct sock *sk2, bool bind_inany)
{
	struct sock_reuseport *old_reuse, *reuse;

	if (!rcu_access_pointer(sk2->sk_reuseport_cb)) {
		int err = reuseport_alloc(sk2, bind_inany);

		if (err)
			return err;
	}

	spin_lock_bh(&reuseport_lock);
	reuse = rcu_dereference_protected(sk2->sk_reuseport_cb,
					  lockdep_is_held(&reuseport_lock));
	old_reuse = rcu_dereference_protected(sk->sk_reuseport_cb,
					     lockdep_is_held(&reuseport_lock));
	if (old_reuse && reuseport_sock_index(sk, old_reuse, true) >= 0) {
		int err = reuseport_resurrect(sk, old_reuse, reuse,
					    reuse->bind_inany);

		spin_unlock_bh(&reuseport_lock);
		return err;
	}
	if (old_reuse && old_reuse->num_socks != 1) {
		spin_unlock_bh(&reuseport_lock);
		return -EBUSY;
	}

	if (reuse->num_socks + reuse->num_closed_socks == reuse->max_socks) {
		reuse = reuseport_grow(reuse);
		if (!reuse) {
			spin_unlock_bh(&reuseport_lock);
			return -ENOMEM;
		}
	}

	__reuseport_add_sock(sk, reuse);
	rcu_assign_pointer(sk->sk_reuseport_cb, reuse);

	spin_unlock_bh(&reuseport_lock);

	if (old_reuse)
		call_rcu(&old_reuse->rcu, reuseport_free_rcu);
	return 0;
}

void reuseport_detach_sock(struct sock *sk)
{
	struct sock_reuseport *reuse;

	spin_lock_bh(&reuseport_lock);
	reuse = rcu_dereference_protected(sk->sk_reuseport_cb,
					  lockdep_is_held(&reuseport_lock));
	if (!reuse)
		goto out;

	/* Notify the bpf side. The sk may be added to a sockarray
	 * map. If so, sockarray logic will remove it from the map.
	 *
	 * Other bpf map types that work with reuseport, like sockmap,
	 * don't need an explicit callback from here. They override sk
	 * unhash/close ops to remove the sk from the map before we
	 * get to this point.
	 */
	bpf_sk_reuseport_detach(sk);

	rcu_assign_pointer(sk->sk_reuseport_cb, NULL);

	if (!__reuseport_detach_closed_sock(sk, reuse))
		__reuseport_detach_sock(sk, reuse);
	if (!reuse->num_socks && !reuse->num_closed_socks)
		call_rcu(&reuse->rcu, reuseport_free_rcu);
out:
	spin_unlock_bh(&reuseport_lock);
}
EXPORT_SYMBOL(reuseport_detach_sock);

void reuseport_stop_listen_sock(struct sock *sk)
{
	struct sock_reuseport *reuse;
	struct bpf_prog *prog;

	if (sk->sk_protocol != IPPROTO_TCP)
		goto detach;
	spin_lock_bh(&reuseport_lock);
	reuse = rcu_dereference_protected(sk->sk_reuseport_cb,
					  lockdep_is_held(&reuseport_lock));
	if (!reuse) {
		spin_unlock_bh(&reuseport_lock);
		return;
	}
	prog = rcu_dereference_protected(reuse->prog,
					 lockdep_is_held(&reuseport_lock));
	if (READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_migrate_req) ||
	    (prog && prog->expected_attach_type ==
		    BPF_SK_REUSEPORT_SELECT_OR_MIGRATE)) {
		bpf_sk_reuseport_detach(sk);
		__reuseport_detach_sock(sk, reuse);
		__reuseport_add_closed_sock(sk, reuse);
		spin_unlock_bh(&reuseport_lock);
		return;
	}
	spin_unlock_bh(&reuseport_lock);
detach:
	reuseport_detach_sock(sk);
}
EXPORT_SYMBOL(reuseport_stop_listen_sock);

static struct sock *run_bpf_filter(struct sock_reuseport *reuse, u16 socks,
				   struct bpf_prog *prog, struct sk_buff *skb,
				   int hdr_len)
{
	struct sk_buff *nskb = NULL;
	u32 index;

	if (skb_shared(skb)) {
		nskb = skb_clone(skb, GFP_ATOMIC);
		if (!nskb)
			return NULL;
		skb = nskb;
	}

	/* temporarily advance data past protocol header */
	if (!pskb_pull(skb, hdr_len)) {
		kfree_skb(nskb);
		return NULL;
	}
	index = bpf_prog_run_save_cb(prog, skb);
	__skb_push(skb, hdr_len);

	consume_skb(nskb);

	if (index >= socks)
		return NULL;

	return reuse->socks[index];
}

static struct sock *reuseport_select_sock_by_hash(struct sock_reuseport *reuse,
						  u32 hash, u16 socks)
{
	int i, start;

	i = start = reciprocal_scale(hash, socks);
	do {
		struct sock *sk = reuse->socks[i];

		if (sk->sk_state != TCP_ESTABLISHED)
			return sk;
		i = (i + 1) % socks;
	} while (i != start);
	return NULL;
}

/**
 *  reuseport_select_sock - Select a socket from an SO_REUSEPORT group.
 *  @sk: First socket in the group.
 *  @hash: When no BPF filter is available, use this hash to select.
 *  @skb: skb to run through BPF filter.
 *  @hdr_len: BPF filter expects skb data pointer at payload data.  If
 *    the skb does not yet point at the payload, this parameter represents
 *    how far the pointer needs to advance to reach the payload.
 *  Returns a socket that should receive the packet (or NULL on error).
 */
struct sock *reuseport_select_sock(struct sock *sk,
				   u32 hash,
				   struct sk_buff *skb,
				   int hdr_len)
{
	struct sock_reuseport *reuse;
	struct bpf_prog *prog;
	struct sock *sk2 = NULL;
	u16 socks;

	rcu_read_lock();
	reuse = rcu_dereference(sk->sk_reuseport_cb);

	/* if memory allocation failed or add call is not yet complete */
	if (!reuse)
		goto out;

	prog = rcu_dereference(reuse->prog);
	socks = READ_ONCE(reuse->num_socks);
	if (likely(socks)) {
		/* paired with smp_wmb() in reuseport_add_sock() */
		smp_rmb();

		if (!prog || !skb)
			goto select_by_hash;

		if (prog->type == BPF_PROG_TYPE_SK_REUSEPORT)
			sk2 = bpf_run_sk_reuseport(reuse, sk, prog, skb,
						 NULL, hash);
		else
			sk2 = run_bpf_filter(reuse, socks, prog, skb, hdr_len);

select_by_hash:
		/* no bpf or invalid bpf result: fall back to hash usage */
		if (!sk2)
			sk2 = reuseport_select_sock_by_hash(reuse, hash, socks);
	}

out:
	rcu_read_unlock();
	return sk2;
}
EXPORT_SYMBOL(reuseport_select_sock);

struct sock *reuseport_migrate_sock(struct sock *sk,
				    struct sock *migrating_sk,
				    struct sk_buff *skb)
{
	struct sock_reuseport *reuse;
	struct bpf_prog *prog;
	struct sock *selected = NULL;
	bool allocated = false;
	u32 hash = migrating_sk->sk_hash;
	u16 socks;

	rcu_read_lock();
	reuse = rcu_dereference(sk->sk_reuseport_cb);
	if (!reuse)
		goto out;
	socks = READ_ONCE(reuse->num_socks);
	if (!socks)
		goto out;
	/* Paired with the write barrier when a listener enters the group. */
	smp_rmb();
	prog = rcu_dereference(reuse->prog);
	if (!prog || prog->expected_attach_type !=
		     BPF_SK_REUSEPORT_SELECT_OR_MIGRATE) {
		if (READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_migrate_req))
			goto select_by_hash;
		goto out;
	}
	if (!skb) {
		skb = alloc_skb(0, GFP_ATOMIC);
		if (!skb)
			goto out;
		allocated = true;
	}
	selected = bpf_run_sk_reuseport(reuse, sk, prog, skb,
					 migrating_sk, hash);
	if (allocated)
		kfree_skb(skb);
select_by_hash:
	if (!selected)
		selected = reuseport_select_sock_by_hash(reuse, hash, socks);
	if (IS_ERR_OR_NULL(selected) ||
	    !refcount_inc_not_zero(&selected->sk_refcnt))
		selected = NULL;
out:
	if (!selected)
		__NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPMIGRATEREQFAILURE);
	rcu_read_unlock();
	return selected;
}
EXPORT_SYMBOL(reuseport_migrate_sock);

int reuseport_attach_prog(struct sock *sk, struct bpf_prog *prog)
{
	struct sock_reuseport *reuse;
	struct bpf_prog *old_prog;

	if (sk_unhashed(sk)) {
		int err;

		if (!sk->sk_reuseport)
			return -EINVAL;

		err = reuseport_alloc(sk, false);

		if (err)
			return err;
	} else if (!rcu_access_pointer(sk->sk_reuseport_cb)) {
		/* The socket wasn't bound with SO_REUSEPORT */
		return -EINVAL;
	}

	spin_lock_bh(&reuseport_lock);
	reuse = rcu_dereference_protected(sk->sk_reuseport_cb,
					  lockdep_is_held(&reuseport_lock));
	old_prog = rcu_dereference_protected(reuse->prog,
					     lockdep_is_held(&reuseport_lock));
	rcu_assign_pointer(reuse->prog, prog);
	spin_unlock_bh(&reuseport_lock);

	sk_reuseport_prog_free(old_prog);
	return 0;
}
EXPORT_SYMBOL(reuseport_attach_prog);

int reuseport_detach_prog(struct sock *sk)
{
	struct sock_reuseport *reuse;
	struct bpf_prog *old_prog;

	old_prog = NULL;
	spin_lock_bh(&reuseport_lock);
	reuse = rcu_dereference_protected(sk->sk_reuseport_cb,
					  lockdep_is_held(&reuseport_lock));
	if (!reuse) {
		spin_unlock_bh(&reuseport_lock);
		return sk->sk_reuseport ? -ENOENT : -EINVAL;
	}

	if (sk_unhashed(sk) && reuse->num_closed_socks) {
		spin_unlock_bh(&reuseport_lock);
		return -ENOENT;
	}

	old_prog = rcu_replace_pointer(reuse->prog, old_prog,
				       lockdep_is_held(&reuseport_lock));
	spin_unlock_bh(&reuseport_lock);

	if (!old_prog)
		return -ENOENT;

	sk_reuseport_prog_free(old_prog);
	return 0;
}
EXPORT_SYMBOL(reuseport_detach_prog);
