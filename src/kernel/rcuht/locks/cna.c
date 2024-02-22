#include <linux/module.h>
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/prefetch.h>
#include <linux/atomic.h>
#include <asm/byteorder.h>
#include <linux/random.h>
#include <linux/topology.h>
#include <linux/sched/clock.h>
#include <linux/moduleparam.h>
#include <linux/sched/rt.h>

#include "spinlock/cna.h"
#include "spinlock/qspinlock_stat.h"

#define CONFIG_NUMA_AWARE_SPINLOCKS 1

/*
 * Implement a NUMA-aware version of MCS (aka CNA, or compact NUMA-aware lock).
 *
 * In CNA, spinning threads are organized in two queues, a primary queue for
 * threads running on the same NUMA node as the current lock holder, and a
 * secondary queue for threads running on other nodes. Schematically, it
 * looks like this:
 *
 *    cna_node
 *   +----------+     +--------+         +--------+
 *   |mcs:next  | --> |mcs:next| --> ... |mcs:next| --> NULL  [Primary queue]
 *   |mcs:locked| -.  +--------+         +--------+
 *   +----------+  |
 *                 `----------------------.
 *                                        v
 *                 +--------+         +--------+
 *                 |mcs:next| --> ... |mcs:next|            [Secondary queue]
 *                 +--------+         +--------+
 *                     ^                    |
 *                     `--------------------'
 *
 * N.B. locked := 1 if secondary queue is absent. Otherwise, it contains the
 * encoded pointer to the tail of the secondary queue, which is organized as a
 * circular list.
 *
 * After acquiring the MCS lock and before acquiring the spinlock, the MCS lock
 * holder checks whether the next waiter in the primary queue (if exists) is
 * running on the same NUMA node. If it is not, that waiter is detached from the
 * main queue and moved into the tail of the secondary queue. This way, we
 * gradually filter the primary queue, leaving only waiters running on the same
 * preferred NUMA node. Note that certain priortized waiters (e.g., in
 * irq and nmi contexts) are excluded from being moved to the secondary queue.
 *
 * We change the NUMA node preference after a waiter at the head of the
 * secondary queue spins for a certain amount of time (1ms, by default).
 * We do that by flushing the secondary queue into the head of the primary queue,
 * effectively changing the preference to the NUMA node of the waiter at the head
 * of the secondary queue at the time of the flush.
 *
 * For more details, see https://arxiv.org/abs/1810.05600.
 *
 * Authors: Alex Kogan <alex.kogan@oracle.com>
 *          Dave Dice <dave.dice@oracle.com>
 */

#define FLUSH_SECONDARY_QUEUE 1

static ulong numa_spinlock_threshold_ns = 1000000; /* 1ms, by default */
module_param(numa_spinlock_threshold_ns, ulong, 0644);

static inline bool intra_node_threshold_reached(struct cna_node *cn)
{
	u64 current_time = local_clock();
	u64 threshold = cn->start_time + numa_spinlock_threshold_ns;

	return current_time > threshold;
}

/*
 * Controls the probability for enabling the ordering of the main queue
 * when the secondary queue is empty. The chosen value reduces the amount
 * of unnecessary shuffling of threads between the two waiting queues
 * when the contention is low, while responding fast enough and enabling
 * the shuffling when the contention is high.
 */
#define SHUFFLE_REDUCTION_PROB_ARG (7)

#define _Q_LOCKED_PENDING_MASK (_Q_LOCKED_MASK | _Q_PENDING_MASK)

/* Per-CPU pseudo-random number seed */
static DEFINE_PER_CPU(u32, seed);

/*
 * Return false with probability 1 / 2^@num_bits.
 * Intuitively, the larger @num_bits the less likely false is to be returned.
 * @num_bits must be a number between 0 and 31.
 */
static bool probably(unsigned int num_bits)
{
	u32 s;

	s = this_cpu_read(seed);
	s = next_pseudo_random32(s);
	this_cpu_write(seed, s);

	return s & ((1 << num_bits) - 1);
}

/*
 * cna_splice_head -- splice the entire secondary queue onto the head of the
 * primary queue.
 *
 * Returns the new primary head node or NULL on failure.
 */
static struct mcs_spinlock *cna_splice_head(struct orig_qspinlock *lock,
					    u32 val, struct mcs_spinlock *node,
					    struct mcs_spinlock *next)
{
	struct mcs_spinlock *head_2nd, *tail_2nd;
	u32 new;

	tail_2nd = cna_decode_tail(node->locked);
	head_2nd = tail_2nd->next;

	if (next) {
		/*
         * If the primary queue is not empty, the primary tail doesn't
         * need to change and we can simply link the secondary tail to
         * the old primary head.
         */
		tail_2nd->next = next;
	} else {
		/*
         * When the primary queue is empty, the secondary tail becomes
         * the primary tail.
         */

		/*
         * Speculatively break the secondary queue's circular link such
         * that when the secondary tail becomes the primary tail it all
         * works out.
         */
		tail_2nd->next = NULL;

		/*
         * tail_2nd->next = NULL;   old = xchg_tail(lock, tail);
         *              prev = cna_decode_tail(old);
         * try_cmpxchg_release(...);    WRITE_ONCE(prev->next, node);
         *
         * If the following cmpxchg() succeeds, our stores will not
         * collide.
         */
		new = ((struct cna_node *)tail_2nd)->encoded_tail |
		      _Q_LOCKED_VAL;
		if (!atomic_try_cmpxchg_release(&lock->val, &val, new)) {
			/* Restore the secondary queue's circular link. */
			tail_2nd->next = head_2nd;
			return NULL;
		}
	}

	/* The primary queue head now is what was the secondary queue head. */
	return head_2nd;
}

static inline bool cna_try_clear_tail(struct orig_qspinlock *lock, u32 val,
				      struct mcs_spinlock *node)
{
	/*
     * We're here because the primary queue is empty; check the secondary
     * queue for remote waiters.
     */
	if (node->locked > 1) {
		struct mcs_spinlock *next;

		/*
         * When there are waiters on the secondary queue, try to move
         * them back onto the primary queue and let them rip.
         */
		next = cna_splice_head(lock, val, node, NULL);
		if (next) {
			arch_mcs_lock_handoff(&next->locked, 1);
			return true;
		}

		return false;
	}

	/* Both queues are empty. Do what MCS does. */
	return __try_clear_tail(lock, val, node);
}

/*
 * cna_splice_next -- splice the next node from the primary queue onto
 * the secondary queue.
 */
static void cna_splice_next(struct mcs_spinlock *node,
			    struct mcs_spinlock *next,
			    struct mcs_spinlock *nnext)
{
	/* remove 'next' from the main queue */
	node->next = nnext;

	/* stick `next` on the secondary queue tail */
	if (node->locked <= 1) { /* if secondary queue is empty */
		struct cna_node *cn = (struct cna_node *)node;

		/* create secondary queue */
		next->next = next;

		cn->start_time = local_clock();
		/* secondary queue is not empty iff start_time != 0 */
		WARN_ON(!cn->start_time);
	} else {
		/* add to the tail of the secondary queue */
		struct mcs_spinlock *tail_2nd = cna_decode_tail(node->locked);
		struct mcs_spinlock *head_2nd = tail_2nd->next;

		tail_2nd->next = next;
		next->next = head_2nd;
	}

	node->locked = ((struct cna_node *)next)->encoded_tail;
}

/*
 * cna_order_queue - check whether the next waiter in the main queue is on
 * the same NUMA node as the lock holder; if not, and it has a waiter behind
 * it in the main queue, move the former onto the secondary queue.
 * Returns 1 if the next waiter runs on the same NUMA node; 0 otherwise.
 */
static int cna_order_queue(struct mcs_spinlock *node)
{
	struct mcs_spinlock *next = READ_ONCE(node->next);
	struct cna_node *cn = (struct cna_node *)node;
	int numa_node, next_numa_node;

	if (!next)
		return 0;

	numa_node = cn->numa_node;
	next_numa_node = ((struct cna_node *)next)->numa_node;

	if (next_numa_node != numa_node &&
	    next_numa_node != CNA_PRIORITY_NODE) {
		struct mcs_spinlock *nnext = READ_ONCE(next->next);

		if (nnext)
			cna_splice_next(node, next, nnext);

		return 0;
	}
	return 1;
}

#define LOCK_IS_BUSY(lock) (atomic_read(&(lock)->val) & _Q_LOCKED_PENDING_MASK)

/* Abuse the pv_wait_head_or_lock() hook to get some work done */
static __always_inline u32 cna_wait_head_or_lock(struct orig_qspinlock *lock,
						 struct mcs_spinlock *node)
{
	struct cna_node *cn = (struct cna_node *)node;

	if (node->locked <= 1 && probably(SHUFFLE_REDUCTION_PROB_ARG)) {
		/*
         * When the secondary queue is empty, skip the calls to
         * cna_order_queue() below with high probability. This optimization
         * reduces the overhead of unnecessary shuffling of threads
         * between waiting queues when the lock is only lightly contended.
         */
		return 0;
	}

	if (!cn->start_time || !intra_node_threshold_reached(cn)) {
		/*
         * We are at the head of the wait queue, no need to use
         * the fake NUMA node ID.
         */
		if (cn->numa_node == CNA_PRIORITY_NODE)
			cn->numa_node = cn->real_numa_node;

		/*
         * Try and put the time otherwise spent spin waiting on
         * _Q_LOCKED_PENDING_MASK to use by sorting our lists.
         */
		while (LOCK_IS_BUSY(lock) && !cna_order_queue(node))
			cpu_relax();
	} else {
		cn->start_time = FLUSH_SECONDARY_QUEUE;
	}

	return 0; /* we lied; we didn't wait, go do so now */
}

static inline void cna_lock_handoff(struct mcs_spinlock *node,
				    struct mcs_spinlock *next)
{
	struct cna_node *cn = (struct cna_node *)node;
	u32 val = 1;

	if (cn->start_time != FLUSH_SECONDARY_QUEUE) {
		if (node->locked > 1) {
			val = node->locked; /* preseve secondary queue */

			/*
             * We have a local waiter, either real or fake one;
             * reload @next in case it was changed by cna_order_queue().
             */
			next = node->next;

			/*
             * Pass over NUMA node id of primary queue, to maintain the
             * preference even if the next waiter is on a different node.
             */
			((struct cna_node *)next)->numa_node = cn->numa_node;

			((struct cna_node *)next)->start_time = cn->start_time;
		}
	} else {
		/*
         * We decided to flush the secondary queue;
         * this can only happen if that queue is not empty.
         */
		WARN_ON(node->locked <= 1);
		/*
         * Splice the secondary queue onto the primary queue and pass the lock
         * to the longest waiting remote waiter.
         */
		next = cna_splice_head(NULL, 0, node, next);
	}

	arch_mcs_lock_handoff(&next->locked, val);
}

#if _Q_PENDING_BITS == 8
/**
 * clear_pending - clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,* -> *,0,*
 */
static __always_inline void clear_pending(struct orig_qspinlock *lock)
{
	WRITE_ONCE(lock->pending, 0);
}

/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,0 -> *,0,1
 *
 * Lock stealing is not allowed if this function is used.
 */
static __always_inline void
clear_pending_set_locked(struct orig_qspinlock *lock)
{
	WRITE_ONCE(lock->locked_pending, _Q_LOCKED_VAL);
}

/*
 * xchg_tail - Put in the new queue tail code word & retrieve previous one
 * @lock : Pointer to queued spinlock structure
 * @tail : The new queue tail code word
 * Return: The previous queue tail code word
 *
 * xchg(lock, tail), which heads an address dependency
 *
 * p,*,* -> n,*,* ; prev = xchg(lock, node)
 */
static __always_inline u32 xchg_tail(struct orig_qspinlock *lock, u32 tail)
{
	/*
     * We can use relaxed semantics since the caller ensures that the
     * MCS node is properly initialized before updating the tail.
     */
	return (u32)xchg_relaxed(&lock->tail, tail >> _Q_TAIL_OFFSET)
	       << _Q_TAIL_OFFSET;
}
#else /* _Q_PENDING_BITS == 8 */

/**
 * clear_pending - clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,* -> *,0,*
 */
static __always_inline void clear_pending(struct orig_qspinlock *lock)
{
	atomic_andnot(_Q_PENDING_VAL, &lock->val);
}

/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,0 -> *,0,1
 */
static __always_inline void
clear_pending_set_locked(struct orig_qspinlock *lock)
{
	atomic_add(-_Q_PENDING_VAL + _Q_LOCKED_VAL, &lock->val);
}

/**
 * xchg_tail - Put in the new queue tail code word & retrieve previous one
 * @lock : Pointer to queued spinlock structure
 * @tail : The new queue tail code word
 * Return: The previous queue tail code word
 *
 * xchg(lock, tail)
 *
 * p,*,* -> n,*,* ; prev = xchg(lock, node)
 */
static __always_inline u32 xchg_tail(struct orig_qspinlock *lock, u32 tail)
{
	u32 old, new, val = atomic_read(&lock->val);

	for (;;) {
		new = (val & _Q_LOCKED_PENDING_MASK) | tail;
		/*
         * We can use relaxed semantics since the caller ensures that
         * the MCS node is properly initialized before updating the
         * tail.
         */
		old = atomic_cmpxchg_relaxed(&lock->val, val, new);
		if (old == val)
			break;

		val = old;
	}
	return old;
}
#endif /* _Q_PENDING_BITS == 8 */

/**
 * queued_fetch_set_pending_acquire - fetch the whole lock value and set pending
 * @lock : Pointer to queued spinlock structure
 * Return: The previous lock value
 *
 * *,*,* -> *,1,*
 */
static __always_inline u32
cna_fetch_set_pending_acquire(struct orig_qspinlock *lock)
{
	return atomic_fetch_or_acquire(_Q_PENDING_VAL, &lock->val);
}

/**
 * set_locked - Set the lock bit and own the lock
 * @lock: Pointer to queued spinlock structure
 *
 * *,*,0 -> *,0,1
 */
static __always_inline void set_locked(struct orig_qspinlock *lock)
{
	WRITE_ONCE(lock->locked, _Q_LOCKED_VAL);
}

/**
 * queued_spin_lock_slowpath - acquire the queued spinlock
 * @lock: Pointer to queued spinlock structure
 * @val: Current value of the queued spinlock 32-bit word
 *
 * (queue tail, pending bit, lock value)
 *
 *              fast     :    slow                                  :    unlock
 *                       :                                          :
 * uncontended  (0,0,0) -:--> (0,0,1) ------------------------------:--> (*,*,0)
 *                       :       | ^--------.------.             /  :
 *                       :       v           \      \            |  :
 * pending               :    (0,1,1) +--> (0,1,0)   \           |  :
 *                       :       | ^--'              |           |  :
 *                       :       v                   |           |  :
 * uncontended           :    (n,x,y) +--> (n,0,0) --'           |  :
 *   queue               :       | ^--'                          |  :
 *                       :       v                               |  :
 * contended             :    (*,x,y) +--> (*,0,0) ---> (*,0,1) -'  :
 *   queue               :         ^--'                             :
 */
void cna_spin_lock_slowpath(struct orig_qspinlock *lock, u32 val)
{
	struct mcs_spinlock *prev, *next, *node;
	u32 old, tail;
	int idx;

	BUILD_BUG_ON(CONFIG_NR_CPUS >= (1U << _Q_TAIL_CPU_BITS));

	/* if (pv_enabled()) */
	/* goto pv_queue; */

	/* if (virt_spin_lock(lock)) */
	/* 	return; */

	/*
	 * Wait for in-progress pending->locked hand-overs with a bounded
	 * number of spins so that we guarantee forward progress.
	 *
	 * 0,1,0 -> 0,0,1
	 */
	if (val == _Q_PENDING_VAL) {
		int cnt = _Q_PENDING_LOOPS;
		val = atomic_cond_read_relaxed(
			&lock->val, (VAL != _Q_PENDING_VAL) || !cnt--);
	}

	/*
	 * If we observe any contention; queue.
	 */
	if (val & ~_Q_LOCKED_MASK)
		goto queue;

	/*
         * trylock || pending
         *
         * 0,0,* -> 0,1,* -> 0,0,1 pending, trylock
         */
	val = cna_fetch_set_pending_acquire(lock);

	/*
         * If we observe contention, there is a concurrent locker.
         *
         * Undo and queue; our setting of PENDING might have made the
         * n,0,0 -> 0,0,0 transition fail and it will now be waiting
         * on @next to become !NULL.
         */
	if (unlikely(val & ~_Q_LOCKED_MASK)) {
		/* Undo PENDING if we set it. */
		if (!(val & _Q_PENDING_MASK))
			clear_pending(lock);

		goto queue;
	}

	/*
         * We're pending, wait for the owner to go away.
         *
         * 0,1,1 -> 0,1,0
         *
         * this wait loop must be a load-acquire such that we match the
         * store-release that clears the locked bit and create lock
         * sequentiality; this is because not all
         * clear_pending_set_locked() implementations imply full
         * barriers.
         */
	if (val & _Q_LOCKED_MASK)
		atomic_cond_read_acquire(&lock->val, !(VAL & _Q_LOCKED_MASK));

	/*
         * take ownership and clear the pending bit.
         *
         * 0,1,0 -> 0,0,1
         */
	clear_pending_set_locked(lock);
	//lockevent_inc(lock_pending);
	return;

	/*
	 * End of pending bit optimistic spinning and beginning of MCS
	 * queuing.
	 */
queue:
	qstat_inc(qstat_lock_slowpath, true);
	/* pv_queue: */
	node = this_cpu_ptr(&qnodes[0].mcs);
	idx = node->count++;
	tail = cna_encode_tail(smp_processor_id(), idx);

	/*
         * 4 nodes are allocated based on the assumption that there will
         * not be nested NMIs taking spinlocks. That may not be true in
         * some architectures even though the chance of needing more than
         * 4 nodes will still be extremely unlikely. When that happens,
         * we fall back to spinning on the lock directly without using
         * any MCS node. This is not the most elegant solution, but is
         * simple enough.
         */
	if (unlikely(idx >= MAX_NODES)) {
		//lockevent_inc(lock_no_node);
		while (!cna_spin_trylock(lock))
			cpu_relax();
		goto release;
	}

	node = grab_mcs_node(node, idx);

	/*
         * Keep counts of non-zero index values:
         */
	//lockevent_cond_inc(lock_use_node2 + idx - 1, idx);

	/*
	 * Ensure that we increment the head node->count before initialising
	 * the actual node. If the compiler is kind enough to reorder these
	 * stores, then an IRQ could overwrite our assignments.
	 */
	barrier();

	node->locked = 0;
	node->next = NULL;
	cna_init_node(node);

	/*
	 * We touched a (possibly) cold cacheline in the per-cpu queue node;
	 * attempt the trylock once more in the hope someone let go while we
	 * weren't watching.
	 */
	if (cna_spin_trylock(lock))
		goto release;

	/*
	 * Ensure that the initialisation of @node is complete before we
	 * publish the updated tail via xchg_tail() and potentially link
	 * @node into the waitqueue via WRITE_ONCE(prev->next, node) below.
	 */
	smp_wmb();

	/*
	 * Publish the updated tail.
	 * We have already touched the queueing cacheline; don't bother with
	 * pending stuff.
	 *
	 * p,*,* -> n,*,*
	 */
	old = xchg_tail(lock, tail);
	next = NULL;

	/*
	 * if there was a previous node; link it and wait until reaching the
	 * head of the waitqueue.
	 */
	if (old & _Q_TAIL_MASK) {
		prev = cna_decode_tail(old);

		/* Link @node into the waitqueue. */
		WRITE_ONCE(prev->next, node);

		//pv_wait_node(node, prev);
		arch_mcs_spin_wait(&node->locked);

		/*
		 * While waiting for the MCS lock, the next pointer may have
		 * been set by another lock waiter. We optimistically load
		 * the next pointer & prefetch the cacheline for writing
		 * to reduce latency in the upcoming MCS unlock operation.
		 */
		next = READ_ONCE(node->next);
		if (next)
			prefetchw(next);
	}

	/*
	 * we're at the head of the waitqueue, wait for the owner & pending to
	 * go away.
	 *
	 * *,x,y -> *,0,0
	 *
	 * this wait loop must use a load-acquire such that we match the
	 * store-release that clears the locked bit and create lock
	 * sequentiality; this is because the set_locked() function below
	 * does not imply a full barrier.
	 *
	 * The PV pv_wait_head_or_lock function, if active, will acquire
	 * the lock and return a non-zero value. So we have to skip the
	 * atomic_cond_read_acquire() call. As the next PV queue head hasn't
	 * been designated yet, there is no way for the locked value to become
	 * _Q_SLOW_VAL. So both the set_locked() and the
	 * atomic_cmpxchg_relaxed() calls will be safe.
	 *
	 * If PV isn't active, 0 will be returned instead.
	 *
	 */
	if ((val = cna_wait_head_or_lock(lock, node)))
		goto locked;

	val = atomic_cond_read_acquire(&lock->val,
				       !(VAL & _Q_LOCKED_PENDING_MASK));

locked:
	/*
	* claim the lock:
	*
	* n,0,0 -> 0,0,1 : lock, uncontended
	* *,*,0 -> *,*,1 : lock, contended
	*
	* If the queue head is the only one in the queue (lock value == tail)
	* and nobody is pending, clear the tail code and grab the lock.
	* Otherwise, we only need to grab the lock.
	*/

	/*
	* In the PV case we might already have _Q_LOCKED_VAL set, because
	* of lock stealing; therefore we must also allow:
	*
	* n,0,1 -> 0,0,1
	*
	* Note: at this point: (val & _Q_PENDING_MASK) == 0, because of the
	*       above wait condition, therefore any concurrent setting of
	*       PENDING will make the uncontended transition fail.
	*/
	if ((val & _Q_TAIL_MASK) == tail) {
		if (cna_try_clear_tail(lock, val, node))
			goto release; /* No contention */
	}

	/*
	* Either somebody is queued behind us or _Q_PENDING_VAL got set
	* which will then detect the remaining tail and queue behind us
	* ensuring we'll see a @next.
	*/
	set_locked(lock);

	/*
	* contended path; wait for next if not observed yet, release.
	*/
	if (!next)
		next = smp_cond_load_relaxed(&node->next, (VAL));

	cna_lock_handoff(node, next);
	//pv_kick_node(lock, next);
release:
	/*
	 * release the node
	 */
	__this_cpu_dec(qnodes[0].mcs.count);
}
