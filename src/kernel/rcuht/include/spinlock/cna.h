#ifndef __CNA_SPINLOCK_H__
#define __CNA_SPINLOCK_H__

#include "qspinlock_i.h"

#include "mcs_spinlock.h"

#include <linux/sched/rt.h>

#define CONFIG_NUMA_AWARE_SPINLOCKS 1

#define MAX_NODES 4

#define CNA_PRIORITY_NODE 0xffff

/*
 * On 64-bit architectures, the mcs_spinlock structure will be 16 bytes in
 * size and four of them will fit nicely in one 64-byte cacheline. For
 * pvqspinlock, however, we need more space for extra data. The same also
 * applies for the NUMA-aware variant of spinlocks (CNA). To accommodate
 * that, we insert two more long words to pad it up to 32 bytes. IOW, only
 * two of them can fit in a cacheline in this case. That is OK as it is rare
 * to have more than 2 levels of slowpath nesting in actual use. We don't
 * want to penalize pvqspinlocks to optimize for a rare case in native
 * qspinlocks.
 */
struct cna_qnode {
	struct mcs_spinlock mcs;
#if defined(CONFIG_PARAVIRT_SPINLOCKS) || defined(CONFIG_NUMA_AWARE_SPINLOCKS)
	long reserved[2];
#endif
};

struct cna_node {
	struct mcs_spinlock mcs;
	u16 numa_node;
	u16 real_numa_node;
	u32 encoded_tail; /* self */
	u64 start_time;
};

/*
 * The pending bit spinning loop count.
 * This heuristic is used to limit the number of lockword accesses
 * made by atomic_cond_read_relaxed when waiting for the lock to
 * transition out of the "== _Q_PENDING_VAL" state. We don't spin
 * indefinitely because there's no guarantee that we'll make forward
 * progress.
 */
#ifndef _Q_PENDING_LOOPS
#define _Q_PENDING_LOOPS 1
#endif

/*
 * Per-CPU queue node structures; we can never have more than 4 nested
 * contexts: task, softirq, hardirq, nmi.
 *
 * Exactly fits one 64-byte cacheline on a 64-bit architecture.
 *
 * PV doubles the storage and uses the second cacheline for PV state.
 * CNA also doubles the storage and uses the second cacheline for
 * CNA-specific state.
 */
static DEFINE_PER_CPU_ALIGNED(struct cna_qnode, qnodes[MAX_NODES]);

#define DEFINE_CNALOCK(x)                                                      \
	struct orig_qspinlock(x) =                                             \
		(struct orig_qspinlock)__ORIG_QSPIN_LOCK_UNLOCKED

/*
 * We must be able to distinguish between no-tail and the tail at 0:0,
 * therefore increment the cpu number by one.
 */

static inline __pure u32 cna_encode_tail(int cpu, int idx)
{
	u32 tail;

	tail = (cpu + 1) << _Q_TAIL_CPU_OFFSET;
	tail |= idx << _Q_TAIL_IDX_OFFSET; /* assume < 4 */

	return tail;
}

static inline __pure struct mcs_spinlock *cna_decode_tail(u32 tail)
{
	int cpu = (tail >> _Q_TAIL_CPU_OFFSET) - 1;
	int idx = (tail & _Q_TAIL_IDX_MASK) >> _Q_TAIL_IDX_OFFSET;

	return per_cpu_ptr(&qnodes[idx].mcs, cpu);
}

static inline __pure struct mcs_spinlock *
grab_mcs_node(struct mcs_spinlock *base, int idx)
{
	return &((struct cna_qnode *)base + idx)->mcs;
}

/*
 * __try_clear_tail - try to clear tail by setting the lock value to
 * _Q_LOCKED_VAL.
 * @lock: Pointer to the queued spinlock structure
 * @val: Current value of the lock
 * @node: Pointer to the MCS node of the lock holder
 */
static __always_inline bool __try_clear_tail(struct orig_qspinlock *lock,
					     u32 val, struct mcs_spinlock *node)
{
	return atomic_try_cmpxchg_relaxed(&lock->val, &val, _Q_LOCKED_VAL);
}

/**
 * queued_spin_is_locked - is the spinlock locked?
 * @lock: Pointer to queued spinlock structure
 * Return: 1 if it is locked, 0 otherwise
 */
static __always_inline int cna_spin_is_locked(struct orig_qspinlock *lock)
{
	/*
	 * Any !0 state indicates it is locked, even if _Q_LOCKED_VAL
	 * isn't immediately observable.
	 */
	return atomic_read(&lock->val);
}

extern void cna_spin_lock_slowpath(struct orig_qspinlock *lock, u32 val);
/**
 * queued_spin_lock - acquire a queued spinlock
 * @lock: Pointer to queued spinlock structure
 */
static __always_inline void cna_spin_lock(struct orig_qspinlock *lock)
{
	u32 val;

	val = atomic_cmpxchg_acquire(&lock->val, 0, _Q_LOCKED_VAL);
	if (likely(val == 0))
		return;
	cna_spin_lock_slowpath(lock, val);
}

static __always_inline void cna_spin_unlock(struct orig_qspinlock *lock)
{
	/*
	 * smp_mb__before_atomic() in order to guarantee release semantics
	 */
	smp_store_release(&lock->locked, 0);
}

/**
 * queued_spin_trylock - try to acquire the queued spinlock
 * @lock : Pointer to queued spinlock structure
 * Return: 1 if lock acquired, 0 if failed
 */
static __always_inline int cna_spin_trylock(struct orig_qspinlock *lock)
{
	if (!atomic_read(&lock->val) &&
	    (atomic_cmpxchg_acquire(&lock->val, 0, _Q_LOCKED_VAL) == 0))
		return 1;
	return 0;
}

static void __init cna_init_nodes_per_cpu(unsigned int cpu)
{
	struct mcs_spinlock *base = per_cpu_ptr(&qnodes[0].mcs, cpu);
	int numa_node = cpu_to_node(cpu);
	int i;

	for (i = 0; i < MAX_NODES; i++) {
		struct cna_node *cn = (struct cna_node *)grab_mcs_node(base, i);

		cn->real_numa_node = numa_node;
		cn->encoded_tail = cna_encode_tail(cpu, i);
		/*
         * make sure @encoded_tail is not confused with other valid
         * values for @locked (0 or 1)
         */
		WARN_ON(cn->encoded_tail <= 1);
	}
}

static void __init cna_init_nodes(void)
{
	unsigned int cpu;

	/*
     * this will break on 32bit architectures, so we restrict
     * the use of CNA to 64bit only (see arch/x86/Kconfig)
     */
	BUILD_BUG_ON(sizeof(struct cna_node) > sizeof(struct cna_qnode));
	/* we store an ecoded tail word in the node's @locked field */
	BUILD_BUG_ON(sizeof(u32) > sizeof(unsigned int));

	for_each_possible_cpu (cpu)
		cna_init_nodes_per_cpu(cpu);
}

static __always_inline void cna_init_node(struct mcs_spinlock *node)
{
	bool priority = !in_task() || irqs_disabled() || rt_task(current);
	struct cna_node *cn = (struct cna_node *)node;

	cn->numa_node = priority ? CNA_PRIORITY_NODE : cn->real_numa_node;
	cn->start_time = 0;
}

#endif /* __CNA_SPINLOCK_H__ */
