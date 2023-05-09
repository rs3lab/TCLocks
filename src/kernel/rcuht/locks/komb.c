// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022 Vishal Gupta, Kumar Kartikeya Dwivedi

/*
 * TODO: (Performance optimiztion)
 * Fix the index part when same is acquired multiple times.
 * Currently nested locking becomes TTAS lock.
 * irqs_disabled() will hurt performance when running in VM.
 */
#ifdef KERNEL_SYNCSTRESS
#include "spinlock/komb.h"
#include "timing_stats.h"
#else
#include <asm-generic/qspinlock.h>
#include <linux/sched.h>
#include <linux/combiner.h>
#define LOCK_START_TIMING_PER_CPU(combiner_loop)
#define LOCK_END_TIMING_PER_CPU(combiner_loop)
#endif
#include <linux/topology.h>
#include <linux/vmalloc.h>

#include <linux/percpu-defs.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>

//#define DSM_DEBUG 1
#ifdef DSM_DEBUG
#define print_debug(fmt, ...)                                                  \
	({                                                                     \
		printk(KERN_EMERG "[%d] komb (%s) lock(%px): " fmt,            \
		       smp_processor_id(), __func__, lock, ##__VA_ARGS__);     \
	})
#else
#define print_debug(fmt, ...)
#endif

#define SIZE_OF_SHADOW_STACK 8192L
#define IRQ_NUMA_NODE 255

//#define DEBUG_KOMB 1

#define UINT64_MAX 0xffffffffffffffffL

#ifdef KERNEL_SYNCSTRESS
#define smp_cond_load_relaxed_sched(ptr, cond_expr)                            \
	({                                                                     \
		typeof(ptr) __PTR = (ptr);                                     \
		__unqual_scalar_typeof(*ptr) VAL;                              \
		for (;;) {                                                     \
			VAL = READ_ONCE(*__PTR);                               \
			if (cond_expr)                                         \
				break;                                         \
			cpu_relax();                                           \
			if (need_resched()) {                                  \
				preempt_enable();                              \
				schedule();                                    \
				preempt_disable();                             \
			}                                                      \
		}                                                              \
		(typeof(*ptr))VAL;                                             \
	})
#else
#define smp_cond_load_relaxed_sched(ptr, cond_expr)                            \
	({                                                                     \
		typeof(ptr) __PTR = (ptr);                                     \
		__unqual_scalar_typeof(*ptr) VAL;                              \
		for (;;) {                                                     \
			VAL = READ_ONCE(*__PTR);                               \
			if (cond_expr)                                         \
				break;                                         \
			cpu_relax();                                           \
		}                                                              \
		(typeof(*ptr))VAL;                                             \
	})

//if (need_resched()) {
//	schedule_preempt_disabled();
//}
#endif

#ifndef smp_cond_load_acquire_sched
#define smp_cond_load_acquire_sched(ptr, cond_expr)                            \
	({                                                                     \
		__unqual_scalar_typeof(*ptr) _val;                             \
		_val = smp_cond_load_relaxed_sched(ptr, cond_expr);            \
		smp_acquire__after_ctrl_dep();                                 \
		(typeof(*ptr))_val;                                            \
	})
#endif

#define atomic_cond_read_acquire_sched(v, c)                                   \
	smp_cond_load_acquire_sched(&(v)->counter, (c))

struct shadow_stack {
	/* 
	 * lock_addr represents the lock addresses acquired by the current CPU.
	 * when acquiring multiple locks on the same CPU (level locking). Each
	 * lock address is stored in the lock_addr array. This helps when locks
	 * are released out of order [Example : Acquire(A), Acquire(B),
	 * Release(A), Release(B)]
	 *
	 * TODO (Space Optimization): Fix this to be at the base of the shadow 
	 * stack.
	 */
	uint64_t lock_addr[8];

	/*
	 * ptr points to the base of the shadow stack.
	 */
	void *ptr;
	/*
 * Used by the combiner to identify for which CPU is the critical section 
 * currently executing.
 */
	uint32_t curr_cs_cpu;
	uint32_t prev_cs_cpu;
	uint64_t counter_val;
	struct komb_node *next_node_ptr;
	uint64_t local_shadow_stack_ptr;
#ifdef NUMA_AWARE
	/*
 * Create a queue which is used by the combiner to store nodes which belong to 
 * different socket.
 */

	struct komb_node *local_queue_head;
	struct komb_node *local_queue_tail;
#endif

	int irqs_disabled;

} __cacheline_aligned_in_smp;

#ifdef LOCK_MEASURE_TIME
static DEFINE_PER_CPU_ALIGNED(uint64_t, combiner_loop);
static DEFINE_PER_CPU_ALIGNED(uint64_t, lock_stack_switch);
static DEFINE_PER_CPU_ALIGNED(uint64_t, unlock_stack_switch);
#endif

#ifdef KOMB_STATS
DEFINE_PER_CPU_ALIGNED(uint64_t, combiner_count);
DEFINE_PER_CPU_ALIGNED(uint64_t, waiter_combined);
DEFINE_PER_CPU_ALIGNED(uint64_t, ooo_combiner_count);
DEFINE_PER_CPU_ALIGNED(uint64_t, ooo_waiter_combined);
DEFINE_PER_CPU_ALIGNED(uint64_t, ooo_unlocks);
#endif

/*
 * Used by all threads to add itself to the queue on the slowpath.
 */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct komb_node, komb_nodes[MAX_NODES]);

/*
 * 8KB shadow stack used by all the threads. For waiter threads, switch to 
 * shadow stack, because of IRQs. For combiner thread, switch to shadow stack 
 * to handle nesting
 */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct shadow_stack, local_shadow_stack);

#define _Q_LOCKED_PENDING_MASK (_Q_LOCKED_MASK | _Q_PENDING_MASK)
#define _Q_LOCKED_COMBINER_VAL 3
#define _Q_UNLOCKED_OOO_VAL 7 //Unlocked a lock out-of-order
#define _Q_LOCKED_IRQ_VAL 15 // Lock Stealing by IRQ
/*
 * We must be able to distinguish between no-tail and the tail at 0:0,
 * therefore increment the cpu number by one.
 */
static inline __pure u32 encode_tail(int cpu, int idx)
{
	u32 tail;

	tail = (cpu + 1) << _Q_TAIL_CPU_OFFSET;
	tail |= idx << _Q_TAIL_IDX_OFFSET; /* assume < 4 */

	return tail;
}

static inline __pure struct komb_node *decode_tail(u32 tail)
{
	int cpu = (tail >> _Q_TAIL_CPU_OFFSET) - 1;
	int idx = (tail & _Q_TAIL_IDX_MASK) >> _Q_TAIL_IDX_OFFSET;

	return per_cpu_ptr(&komb_nodes[idx], cpu);
}

inline __pure u32 get_cpu_from_tail(u32 tail)
{
	return ((tail >> _Q_TAIL_CPU_OFFSET) - 1);
}

static inline bool check_irq_node(struct komb_node *node)
{
	return (node->socket_id == IRQ_NUMA_NODE || node->rsp == 0xdeadbeef);
}

__always_inline void clear_locked_set_completed(struct komb_node *lock)
{
	WRITE_ONCE(lock->locked_completed, 1);
}

__always_inline void clear_pending_set_locked(struct qspinlock *lock)
{
#ifdef DEBUG_KOMB
	BUG_ON(lock->locked != 0);
#endif
	WRITE_ONCE(lock->locked_pending, _Q_LOCKED_VAL);
}

static __always_inline u32 xchg_tail(struct qspinlock *lock, u32 tail)
{
	return ((u32)xchg(&lock->tail, tail >> _Q_TAIL_OFFSET))
	       << _Q_TAIL_OFFSET;
}

__always_inline u32 cmpxchg_tail(struct qspinlock *lock, u32 tail, u32 new_tail)
{
	return ((u32)cmpxchg(&lock->tail, tail >> _Q_TAIL_OFFSET,
			     new_tail >> _Q_TAIL_OFFSET))
	       << _Q_TAIL_OFFSET;
}

/**
 * clear_pending - clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,* -> *,0,*
 */
static __always_inline void clear_pending(struct qspinlock *lock)
{
	atomic_andnot(_Q_PENDING_VAL, &lock->val);
}

/**
 * set_locked - Set the lock bit and own the lock
 * @lock: Pointer to queued spinlock structure
 *
 * *,*,0 -> *,0,1
 */
static __always_inline void set_locked(struct qspinlock *lock)
{
	WRITE_ONCE(lock->locked, _Q_LOCKED_VAL);
}

static __always_inline void check_and_set_combiner(struct qspinlock *lock)
{
	u32 val, new_val;

#ifdef DEBUG_KOMB
	BUG_ON(lock->locked == _Q_LOCKED_VAL);
#endif

	if (lock->locked > 0) {
		while (true) {
			val = atomic_cond_read_relaxed(
				&lock->val, !(VAL & _Q_LOCKED_PENDING_MASK));

#ifdef DEBUG_KOMB
			BUG_ON(lock->locked != 0);
#endif

			new_val = val >> _Q_LOCKED_BITS;
			new_val <<= _Q_LOCKED_BITS;
			new_val |= _Q_LOCKED_COMBINER_VAL;

			if (atomic_cmpxchg_acquire(&lock->val, val, new_val) ==
			    val)
				return;
		}
	}
	WRITE_ONCE(lock->locked, _Q_LOCKED_COMBINER_VAL);
}

/**
 * queued_fetch_set_pending_acquire - fetch the whole lock value and set pending
 * @lock : Pointer to queued spinlock structure
 * Return: The previous lock value
 *
 * *,*,* -> *,1,*
 */
static __always_inline u32 komb_fetch_set_pending_acquire(struct qspinlock *lock)
{
	return atomic_fetch_or_acquire(_Q_PENDING_VAL, &lock->val);
}

#ifdef NUMA_AWARE
__always_inline static void add_to_local_queue(struct komb_node *node)
{
	//printk(KERN_ALERT "%d Move node to local queue: %d\n",
	//     smp_processor_id(), node->cpuid);

	struct shadow_stack *ptr = this_cpu_ptr(&local_shadow_stack);

	if (ptr->local_queue_head == NULL) {
		ptr->local_queue_head = node;
		ptr->local_queue_tail = node;
	} else {
		ptr->local_queue_tail->next = node;
		ptr->local_queue_tail = node;
	}
}
#endif

__always_inline static struct komb_node *
get_next_node(struct komb_node *my_node)
{
#ifdef NUMA_AWARE
	struct komb_node *curr_node, *next_node;

	curr_node = my_node;
	next_node = curr_node->next;

	while (true) {
		if (next_node == NULL || next_node->next == NULL)
			goto next_node_null;
		else if (check_irq_node(next_node) ||
			 check_irq_node(next_node->next))
			goto next_node_null;

		if (next_node->socket_id == numa_node_id()) {
#ifdef PREFETCHING
			void *rsp_ptr =
				(per_cpu_ptr(&komb_nodes[0], next_node->cpuid)
					 ->rsp);
			prefetchw(rsp_ptr);
			int i;
			for (i = 1; i < NUM_PREFETCH_LINES; i++)
				prefetchw(rsp_ptr + (64 * i));

			prefetch(next_node->next);
#endif
			return next_node;
		}

		add_to_local_queue(next_node);
		curr_node = next_node;
		next_node = curr_node->next;
	}

next_node_null:
	return next_node;
#else
	return my_node->next;
#endif
}

#pragma GCC push_options
#pragma GCC optimize("O3")

__attribute__((noipa)) noinline notrace static void
execute_cs(struct qspinlock *lock, struct komb_node *curr_node)
{
	void *incoming_rsp_ptr, *outgoing_rsp_ptr;
	struct komb_node *next_node = NULL;

	struct shadow_stack *ptr = this_cpu_ptr(&local_shadow_stack);

	ptr->curr_cs_cpu = curr_node->cpuid;

#ifdef DEBUG_KOMB
	BUG_ON(curr_node->cpuid == smp_processor_id());
	if ((ptr->ptr - (ptr->local_shadow_stack_ptr)) > SIZE_OF_SHADOW_STACK) {
		printk(KERN_ALERT "%d %px %px\n", smp_processor_id(), ptr->ptr,
		       ptr->local_shadow_stack_ptr);
		BUG_ON(true);
	}
#endif

	incoming_rsp_ptr = &(curr_node->rsp);
	outgoing_rsp_ptr = &(ptr->local_shadow_stack_ptr);

#ifdef LOCK_MEASURE_TIME
	*this_cpu_ptr(&combiner_loop) = UINT64_MAX;
#endif

	/*
	 * Make the actual switch, the pushed return address is after this
	 * function call, which we will resume execution at using the switch
	 * in unlock.
	 */
#ifdef DEBUG_KOMB
#ifdef ENABLE_IRQS_CHECK
	BUG_ON(irqs_disabled());
#endif

	BUG_ON(*(uint64_t *)incoming_rsp_ptr == NULL);
	BUG_ON(*(uint64_t *)outgoing_rsp_ptr == NULL);
#endif
	//local_irq_disable();
	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
	//local_irq_enable();

#ifdef DEBUG_KOMB
	if ((ptr->ptr - (ptr->local_shadow_stack_ptr)) > SIZE_OF_SHADOW_STACK) {
		printk(KERN_ALERT "%d %px %px\n", smp_processor_id(), ptr->ptr,
		       ptr->local_shadow_stack_ptr);
		BUG_ON(true);
	}
	BUG_ON(ptr->ptr - (ptr->local_shadow_stack_ptr) > SIZE_OF_SHADOW_STACK);
#endif

	if (lock->locked == _Q_UNLOCKED_OOO_VAL) {
		print_debug("Combiner got control back OOO unlock\n");

#ifdef KOMB_STATS
		this_cpu_add(ooo_waiter_combined, ptr->counter_val);
		this_cpu_inc(ooo_combiner_count);
#endif

		if (ptr->curr_cs_cpu != -1) {
			print_debug("OOO waking up %d\n", ptr->curr_cs_cpu);
			curr_node =
				per_cpu_ptr(&komb_nodes[0], ptr->curr_cs_cpu);
			curr_node->rsp = this_cpu_ptr(&komb_nodes[0])->rsp;
			clear_locked_set_completed(curr_node);
#ifdef DEBUG_KOMB
			BUG_ON(ptr->prev_cs_cpu != -1);
#endif
		}
		ptr->prev_cs_cpu = -1;
		ptr->curr_cs_cpu = -1;
		lock->locked = _Q_LOCKED_COMBINER_VAL;

		next_node = ptr->next_node_ptr;

		if (next_node != NULL && next_node->next != NULL &&
		    !check_irq_node(next_node) &&
		    !check_irq_node(next_node->next)) {
			execute_cs(lock, ptr->next_node_ptr);
		}
	}

	//clear_locked_set_completed(curr_node);
}
#pragma GCC pop_options

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace static void
run_combiner(struct qspinlock *lock, struct komb_node *curr_node)
{
#ifdef DEBUG_KOMB
	BUG_ON(curr_node == NULL);
#endif
	struct komb_node *next_node = curr_node->next;
	int counter = 0;

	if (next_node == NULL || check_irq_node(curr_node) ||
	    check_irq_node(next_node)) {
		set_locked(lock);
		/*
		 * Make this node spin on the locked variable and then it will 
		 * become the combiner.
		 */
		curr_node->locked = false;
		smp_mb();
		return;
	}

	struct shadow_stack *ptr = this_cpu_ptr(&local_shadow_stack);

#ifndef WWJUMP
	while (curr_node) {
		counter++;

		next_node = get_next_node(curr_node);

		execute_cs(lock, curr_node);

		clear_locked_set_completed(curr_node);

		if (next_node == NULL || next_node->next == NULL ||
		    counter >= komb_batch_size)
			break;

		curr_node = next_node;
	}
#else
	ptr->counter_val = 0;

	print_debug("Combiner %d giving control to %d\n", smp_processor_id(),
		    curr_node->cpuid);

	execute_cs(lock, curr_node);

	print_debug(
		"Combiner got the control back: %d counter: %d last_waiter: %d\n",
		smp_processor_id(), ptr->counter_val, ptr->curr_cs_cpu);

#ifdef KOMB_STATS
	this_cpu_add(waiter_combined, ptr->counter_val);
	this_cpu_inc(combiner_count);
#endif

	if (ptr->prev_cs_cpu != -1) {
		clear_locked_set_completed(
			per_cpu_ptr(&komb_nodes[0], ptr->prev_cs_cpu));
		ptr->prev_cs_cpu = -1;
	}

	next_node = ptr->next_node_ptr;
#endif

#ifdef NUMA_AWARE
	if (ptr->local_queue_head != NULL) {
		ptr->local_queue_tail->next = next_node;
		next_node = ptr->local_queue_head;
		ptr->local_queue_head = NULL;
		ptr->local_queue_tail = NULL;
	}
#endif

	print_debug("After combiner %d, next node: %d\n", smp_processor_id(),
		    next_node->cpuid);

#ifdef DEBUG_KOMB
	BUG_ON(next_node == NULL);
#endif

	set_locked(lock);

#ifdef WWJUMP
	ptr->curr_cs_cpu = -1;
#endif
	/* 
	 * Make this node spin on the locked variable and then it will become 
	 * the combiner.
	 */
	next_node->locked = false;
}
#pragma GCC pop_options

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace static int
__komb_spin_lock_longjmp(struct qspinlock *lock, int tail,
			 register struct komb_node *curr_node)
{
	register struct komb_node *prev_node = NULL, *next_node = NULL;
	struct qspinlock *parent_lock;
	int old_tail, val, j;

	/*
	 * TODO: Make sure these variables are stored on the combiner stack
	 * so that they can be restored later.
	 * These variables are needed when there is a waiter or a combiner
	 * running within a combiner (For same or different lock).
	 */
	uint32_t prev_cs_cpu;
	bool prev_locked_val;
	uint64_t prev_rsp;
	uint64_t prev_counter_val;
	struct komb_node *prev_next_node_ptr = NULL;
#ifdef NUMA_AWARE
	struct komb_node *prev_local_queue_head;
	struct komb_node *prev_local_queue_tail;
#endif

	old_tail = xchg_tail(lock, tail);

	if (old_tail & _Q_TAIL_MASK) {
		prev_node = decode_tail(old_tail);

		prev_node->next = curr_node;

		int count = 0;

		smp_cond_load_relaxed_sched(&curr_node->locked, !(VAL));

		/*while (READ_ONCE(curr_node->locked)) {
			cpu_relax();
			count++;
			if (count == INT_MAX)
				BUG_ON(true);
		}*/

		struct shadow_stack *ptr = this_cpu_ptr(&local_shadow_stack);

		if (curr_node->completed) {
#ifdef ENABLE_IRQS_CHECK
			if (curr_node->irqs_disabled) {
				//local_irq_disable();
				ptr->irqs_disabled = curr_node->irqs_disabled;
			}
#endif
			for (j = 7; j >= 0; j--)
				if (ptr->lock_addr[j] != NULL)
					break;

			curr_node->count--;

			if (j >= 0) {
				parent_lock = ptr->lock_addr[j];
#ifdef DEBUG_KOMB
				BUG_ON(parent_lock == lock);
#endif
				if (parent_lock->locked ==
				    _Q_UNLOCKED_OOO_VAL) {
					print_debug("Waiter unlocked OOO\n");
					return 1;
				}
			}

			prefetchw(curr_node->rsp);
			prefetchw(curr_node->rsp + 64);
			prefetchw(curr_node->rsp + 128);
			prefetchw(curr_node->rsp + 192);
			prefetchw(curr_node->rsp + 256);
			prefetchw(curr_node->rsp + 320);
			LOCK_START_TIMING_PER_CPU(unlock_stack_switch);
			return 0;
		}
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
	 */

	val = atomic_cond_read_acquire(&lock->val,
				       !(VAL & _Q_LOCKED_PENDING_MASK));

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

	if (((val & _Q_TAIL_MASK) == tail) &&
	    atomic_try_cmpxchg_relaxed(&lock->val, &val, _Q_LOCKED_VAL))
		goto release; /* No contention */

	/* Either somebody is queued behind us or _Q_PENDING_VAL is set */
	check_and_set_combiner(lock);

	/*
	 * contended path; wait for next if not observed yet, release.
	 */
	smp_cond_load_relaxed_sched(&curr_node->next, (VAL));
	next_node = curr_node->next;
#ifdef DEBUG_KOMB
	BUG_ON(next_node == NULL);
#endif
	/*while (!next_node) {
		next_node = curr_node->next;
		cpu_relax();
	}*/

	struct shadow_stack *ptr = this_cpu_ptr(&local_shadow_stack);

	prev_cs_cpu = ptr->curr_cs_cpu;
	ptr->curr_cs_cpu = -1;

	ptr->irqs_disabled = false;

#ifdef DEBUG_KOMB
	BUG_ON(ptr->prev_cs_cpu != -1);
#endif
	/*
	 * curr_node at the head of the queue. 
	 * Release the node and run_combiner.
	 */
	curr_node->count--;
	prev_locked_val = lock->locked;
#ifdef DEBUG_KOMB
	BUG_ON(prev_locked_val >= _Q_LOCKED_COMBINER_VAL);
#endif
	lock->locked = _Q_LOCKED_COMBINER_VAL;
	prev_rsp = curr_node->rsp;
	curr_node->rsp = NULL;

	prev_counter_val = ptr->counter_val;
	ptr->counter_val = 0;

	prev_next_node_ptr = ptr->next_node_ptr;
	ptr->next_node_ptr = NULL;

#ifdef NUMA_AWARE
	prev_local_queue_head = ptr->local_queue_head;
	prev_local_queue_tail = ptr->local_queue_tail;

	ptr->local_queue_head = NULL;
	ptr->local_queue_tail = NULL;
#endif

	j = 7;
	for (j = 7; j >= 0; j--)
		if (ptr->lock_addr[j] != NULL)
			break;
	j += 1;
#ifdef DEBUG_KOMB
	BUG_ON(j >= 8 || j < 0);
#endif
	ptr->lock_addr[j] = lock;

	run_combiner(lock, next_node);

#ifdef DEBUG_KOMB
	BUG_ON(this_cpu_ptr(&local_shadow_stack)->lock_addr[j] != lock);
#endif

	ptr->lock_addr[j] = NULL;

	ptr->next_node_ptr = prev_next_node_ptr;
	ptr->counter_val = prev_counter_val;

#ifdef NUMA_AWARE
	ptr->local_queue_head = prev_local_queue_head;
	ptr->local_queue_tail = prev_local_queue_tail;
#endif

	curr_node->rsp = prev_rsp;

	if (lock->locked == _Q_UNLOCKED_OOO_VAL) {
		if (prev_cs_cpu != -1) {
			print_debug("Waking up %d\n", prev_cs_cpu);
			clear_locked_set_completed(
				per_cpu_ptr(&komb_nodes[0], prev_cs_cpu));
		}
		ptr->curr_cs_cpu = -1;
	} else {
		ptr->curr_cs_cpu = prev_cs_cpu;
	}
	lock->locked = prev_locked_val;

	return 0;
release:
	/* 
	 * release the node
	 */
	curr_node->count--;
	return 0;
}
#pragma GCC pop_options

__attribute__((noipa)) noinline notrace static int
__komb_spin_lock_slowpath(struct qspinlock *lock)
{
	struct komb_node *curr_node;
	int tail, idx;

	LOCK_END_TIMING_PER_CPU(lock_stack_switch);

	curr_node = this_cpu_ptr(&komb_nodes[0]);
	idx = curr_node->count++;
	tail = encode_tail(smp_processor_id(), idx);

	/*
	 * Initialize curr_node
	 */
	curr_node->locked = true;
	curr_node->completed = false;
	curr_node->next = NULL;
	curr_node->tail = tail;
	curr_node->socket_id = numa_node_id();
	curr_node->cpuid = smp_processor_id();
	curr_node->irqs_disabled = false;
	curr_node->lock = lock;
	curr_node->task_struct_ptr = current;

	return __komb_spin_lock_longjmp(lock, tail, curr_node);
}

/*
 * Public API
 */

void komb_init(void)
{
	int i, j;
	for_each_possible_cpu (i) {
		void *stack_ptr = vzalloc(SIZE_OF_SHADOW_STACK);
		struct shadow_stack *ptr = per_cpu_ptr(&local_shadow_stack, i);

#ifdef DEBUG_KOMB
		BUG_ON(stack_ptr == NULL);
#endif

		ptr->ptr = stack_ptr + SIZE_OF_SHADOW_STACK;
		for (j = 0; j < 8; j++)
			ptr->lock_addr[j] = 0;
		ptr->local_shadow_stack_ptr =
			stack_ptr + SIZE_OF_SHADOW_STACK - 8;
		ptr->curr_cs_cpu = -1;
		ptr->prev_cs_cpu = -1;
		ptr->counter_val = 0;
		ptr->next_node_ptr = NULL;
#ifdef NUMA_AWARE
		ptr->local_queue_head = NULL;
		ptr->local_queue_tail = NULL;
#endif

		ptr->irqs_disabled = false;

#ifdef LOCK_MEASURE_TIME
		*per_cpu_ptr(&combiner_loop, i) = UINT64_MAX;
#endif
	}
}

void komb_free(void)
{
	int i;
	for_each_possible_cpu (i) {
		vfree(per_cpu_ptr(&local_shadow_stack, i)->ptr -
		      SIZE_OF_SHADOW_STACK);
	}
}

void komb_spin_lock_init(struct qspinlock *lock)
{
	atomic_set(&lock->val, 0);
}

__attribute__((noipa)) noinline notrace static uint64_t
get_shadow_stack_ptr(void)
{
	struct shadow_stack *ptr = this_cpu_ptr(&local_shadow_stack);

	return &(ptr->local_shadow_stack_ptr);
}

__attribute__((noipa)) noinline notrace static struct komb_node *
get_komb_node(void)
{
	return this_cpu_ptr(&komb_nodes[0]);
}

/* 
 * When the following function is called, the compiler in caller frame does
 * this sequence:
 *
 * push caller-saved-regs			(rdi, rsi, rdx, rcx, ...)
 * ...<prepare arguments>...
 * .- push eip+1				(eip + 1 == pop insn)
 * callq |
 * `- jmp komb_spin_lock
 * pop caller-saved-regs
 *
 * This means that the following function must in no way modify the the rsp
 * before it is written to the task_frame, otherwise the top of the stack
 * (rsp) would no longer point to the return address. To do this, emit a
 * call in assembly and pass the top of the stack as an argument so that
 * we can use the stack while preserving the semantics of the combiner.
 *
 * NOTE: One advantage of offloading the caller-saved register spill to the
 * compiler is that it only saves the caller-saved registers in use in the
 * caller frame. If we were to inline this whole logic in assembly, we wouldn't
 * know the registers in use and would have to save all possible caller-saved
 * registers.
 */

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace void
komb_spin_lock_slowpath(struct qspinlock *lock)
{
	register int ret_val;
	/* 
	 * We must save the callee-saved registers now, since we will resume
	 * execution at the return address currently at top of stack. If we
	 * defer saving to when context switch happens later, the C function
	 * may have saved callee-saved registers and will restore them before
	 * returning.
	 *
	 * Due to unstructured control flow, do the saving now.
	 *
	 * NOTE: When we touch the stack inside this function, so a 16-byte
	 * stack's alignment will change to 8-byte alignment when callq pushes
	 * our return address, but Linux only assumes 8-byte stack alignment,
	 * so it isn't a problem for us.
	 */
#ifdef KOMB_DEBUG
#ifdef ENABLE_IRQS_CHECK
	BUG_ON(irqs_disabled());
#endif
#endif
	//local_irq_disable();
	asm volatile("pushq %%rbp\n"
		     "pushq %%rbx\n"
		     "pushq %%r12\n"
		     "pushq %%r13\n"
		     "pushq %%r14\n"
		     "pushq %%r15\n"
		     :
		     :
		     : "memory");
	asm volatile("callq %P0\n"
		     "movq %%rsp, %c1(%%rax)\n"
		     :
		     : "i"(get_komb_node), "i"(offsetof(struct komb_node, rsp))
		     : "memory");
	asm volatile("callq %P0\n"
		     "movq (%%rax), %%rsp\n"
		     :
		     : "i"(get_shadow_stack_ptr)
		     : "memory");
	//local_irq_enable();

	ret_val = __komb_spin_lock_slowpath(lock);

	//local_irq_disable();
	if (ret_val) {
		asm volatile("callq %P0\n"
			     "movq (%%rax), %%rsp\n"
			     "popq %%r15\n"
			     "popq %%r14\n"
			     "popq %%r13\n"
			     "popq %%r12\n"
			     "popq %%rbx\n"
			     "popq %%rbp\n"
			     "retq\n"
			     :
			     : "i"(get_shadow_stack_ptr)
			     : "memory");
	} else {
		asm volatile("callq %P0\n"
			     "movq %%rsp, (%%rax)\n"
			     :
			     : "i"(get_shadow_stack_ptr)
			     : "memory");
		asm volatile("callq %P0\n"
			     "movq %c1(%%rax), %%rsp\n"
			     :
			     : "i"(get_komb_node),
			       "i"(offsetof(struct komb_node, rsp))
			     : "memory");
		asm volatile("popq %%r15\n"
			     "popq %%r14\n"
			     "popq %%r13\n"
			     "popq %%r12\n"
			     "popq %%rbx\n"
			     "popq %%rbp\n"
			     "retq\n"
			     :
			     :
			     : "memory");
	}
	//local_irq_enable();
}
#pragma GCC pop_options

__attribute__((noipa)) noinline notrace void
komb_spin_lock(struct qspinlock *lock)
{
	u32 val, cnt;
	struct komb_node *curr_node = NULL;

	/*
	 * Fastpath
	 */

	val = atomic_cmpxchg_acquire(&lock->val, 0, _Q_LOCKED_VAL);
	if (val == 0)
		return;

	/*
	 * Mid-path
	 *
	 * Wait for in-progress pending->locked hand-overs with a bounded
	 * number of spins so that we guarantee forward progress.
	 *
	 * 0,1,0 -> 0,0,1
	 */
	if (val == _Q_PENDING_VAL) {
		cnt = _Q_PENDING_LOOPS;
		val = atomic_cond_read_relaxed(
			&lock->val, (VAL != _Q_PENDING_VAL) || !cnt--);
	}

	/*
	 * If we observe any contention; queue.
	 */
	if (val & ~_Q_LOCKED_MASK ||
	    (val & _Q_LOCKED_MASK) == _Q_LOCKED_IRQ_VAL)
		goto queue;

	/*
     	* trylock || pending
     	*
     	* 0,0,* -> 0,1,* -> 0,0,1 pending, trylock
     	*/
	val = komb_fetch_set_pending_acquire(lock);

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
	return;

	/*
	 * End of pending bit optimistic spinning and beginning of MCS
	 * queuing.
	 */
queue:

	curr_node = this_cpu_ptr(&komb_nodes[0]);
#ifdef DEBUG_KOMB
	BUG_ON(curr_node == NULL);
#endif
#ifdef ENABLE_IRQS_CHECK
	if (curr_node->count > 0 || !in_task() || irqs_disabled() ||
	    current->migration_disabled) {
		/*print_debug("Nested lock: waiting for lock\n");
		while (true) {
			atomic_cond_read_acquire(&lock->val, !(VAL));

			if (atomic_cmpxchg_acquire(&lock->val, 0,
						   _Q_LOCKED_IRQ_VAL) == 0)
				break;
		}
		print_debug("Nested lock: got the lock\n");
		return;
	} else if (!in_task() ||irqs_disabled() || current->migration_disabled) {*/
		struct komb_node *prev_node, *next_node;
		u32 tail, idx;

		idx = curr_node->count++;
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
			while (!komb_spin_trylock(lock))
				cpu_relax();
			goto irq_release;
		}

		curr_node += idx;
		tail = encode_tail(smp_processor_id(), idx);

		barrier();

		curr_node->locked = true;
		curr_node->completed = false;
		curr_node->next = NULL;
		curr_node->tail = tail;
		curr_node->socket_id = IRQ_NUMA_NODE;
		curr_node->cpuid = smp_processor_id();
		curr_node->irqs_disabled = false;
		curr_node->lock = lock;
		curr_node->task_struct_ptr = current;

		uint64_t prev_rsp = curr_node->rsp;
		curr_node->rsp = 0xdeadbeef;

		smp_wmb();

		u32 old_tail = xchg_tail(lock, tail);

		if (old_tail & _Q_TAIL_MASK) {
			prev_node = decode_tail(old_tail);
			WRITE_ONCE(prev_node->next, curr_node);

			print_debug("IRQ going to waiting for lock\n");
			smp_cond_load_relaxed_sched(&curr_node->locked, !(VAL));
		}

		curr_node->rsp = prev_rsp;

		u32 val, new_val;

		print_debug("IRQ spinning on the locked field\n");

		val = atomic_cond_read_acquire(&lock->val,
					       !(VAL & _Q_LOCKED_PENDING_MASK));

		if (((val & _Q_TAIL_MASK) == tail) &&
		    atomic_try_cmpxchg_relaxed(&lock->val, &val,
					       _Q_LOCKED_IRQ_VAL)) {
			print_debug("IRQ only one in the queue unlocked\n");
			goto irq_release;
		}

		//set_locked(lock);

		while (true) {
			val = atomic_cond_read_relaxed(
				&lock->val, !(VAL & _Q_LOCKED_PENDING_MASK));

#ifdef DEBUG_KOMB
			BUG_ON(lock->locked != 0);
#endif

			new_val = val >> _Q_LOCKED_BITS;
			new_val <<= _Q_LOCKED_BITS;
			new_val |= _Q_LOCKED_IRQ_VAL;

			if (atomic_cmpxchg_acquire(&lock->val, val, new_val) ==
			    val)
				break;
		}

		print_debug("IRQ got the lock\n");

		smp_cond_load_relaxed_sched(&curr_node->next, (VAL));
		next_node = curr_node->next;

#ifdef DEBUG_KOMB
		BUG_ON(next_node == NULL);
#endif

		WRITE_ONCE(next_node->locked, false);

		print_debug("IRQ passing lock next node: %d\n",
			    next_node->cpuid);

	irq_release:
		curr_node = this_cpu_ptr(&komb_nodes[0]);
		curr_node->count--;
		return;
	} else {
#endif

#ifdef LOCK_MEASURE_TIME
		*this_cpu_ptr(&lock_stack_switch) = UINT64_MAX;
		*this_cpu_ptr(&unlock_stack_switch) = UINT64_MAX;
#endif
		LOCK_START_TIMING_PER_CPU(lock_stack_switch);

		komb_spin_lock_slowpath(lock);

#ifdef WWJUMP
		struct shadow_stack *ptr = this_cpu_ptr(&local_shadow_stack);

		uint32_t curr_cpuid = ptr->curr_cs_cpu;
		if (curr_cpuid != -1) {
			struct komb_node *curr_node =
				per_cpu_ptr(&komb_nodes[0], curr_cpuid);
			if (curr_node->lock == lock) {
#ifdef DEBUG_KOMB
				BUG_ON(lock->locked != _Q_LOCKED_COMBINER_VAL);
#endif
				struct komb_node *next_node =
					get_next_node(curr_node);
				ptr->next_node_ptr = next_node;
			}

			uint32_t prev_cpuid = ptr->prev_cs_cpu;
			if (prev_cpuid != -1) {
				struct komb_node *prev_node =
					per_cpu_ptr(&komb_nodes[0], prev_cpuid);

#ifdef DEBUG_KOMB
				BUG_ON(prev_node->lock != lock);
#endif
				print_debug("Waking up prev waiter: %d\n",
					    prev_cpuid);
				clear_locked_set_completed(prev_node);
				ptr->prev_cs_cpu = -1;

				//	LOCK_END_TIMING_PER_CPU(combiner_loop);
			}
		}
#endif

#ifdef ENABLE_IRQS_CHECK
	}
#endif
}
EXPORT_SYMBOL_GPL(komb_spin_lock);

struct task_struct *komb_get_current(spinlock_t *lock)
{
	struct shadow_stack *ptr = this_cpu_ptr(&local_shadow_stack);

	int j, my_idx;

	j = 0;
	my_idx = -1;

	for (j = 0; j < 8; j++) {
		if (ptr->lock_addr[j] == lock) {
#ifdef DEBUG_KOMB
			BUG_ON(ptr->curr_cs_cpu < 0);
#endif
			return per_cpu_ptr(&komb_nodes[0], ptr->curr_cs_cpu)
				->task_struct_ptr;
		}
	}

	return current;
}

void komb_set_current_state(spinlock_t *lock, unsigned int state)
{
	WRITE_ONCE(komb_get_current(lock)->__state, state);
	smp_mb();
}

/* 
 * No IPA is even more relevant here, because even if assembly is removed,
 * compiler shouldn't assume which caller-saved registers are in use inside
 * the function. When we jump from the spin loop to the return address after
 * the unlock function, we have caller-saved registers that are completely
 * inconsistent, because we only restored the callee-saved registers.
 */
__attribute__((noipa)) noinline notrace void
komb_spin_unlock(struct qspinlock *lock)
{
	struct shadow_stack *ptr = this_cpu_ptr(&local_shadow_stack);
	int from_cpuid = ptr->curr_cs_cpu;
	void *incoming_rsp_ptr, *outgoing_rsp_ptr;

	int j, max_idx, my_idx;

	uint64_t temp_lock_addr;

	j = 0;
	max_idx = -1;
	my_idx = -1;

	if (lock->locked == _Q_LOCKED_VAL ||
	    lock->locked == _Q_LOCKED_IRQ_VAL) {
		lock->locked = false;
		return;
	}

	for (j = 0; j < 8; j++) {
		temp_lock_addr = ptr->lock_addr[j];
		if (temp_lock_addr != NULL)
			max_idx = j;
		if (temp_lock_addr == lock)
			my_idx = j;
		if (temp_lock_addr == NULL)
			break;
	}

	if (my_idx == -1) {
		if (lock->locked == _Q_LOCKED_VAL ||
		    lock->locked == _Q_LOCKED_IRQ_VAL)
			lock->locked = false;
		else if (lock->locked == _Q_LOCKED_COMBINER_VAL) {
#ifdef KOMB_STATS
			this_cpu_inc(ooo_unlocks);
#endif
			lock->locked = _Q_UNLOCKED_OOO_VAL;
			print_debug("OOO unlock\n");
		} else
			BUG_ON(true);
		return;
	}

	LOCK_END_TIMING_PER_CPU_DISABLE(combiner_loop);

	LOCK_START_TIMING_PER_CPU_DISABLE(combiner_loop);

#ifdef DEBUG_KOMB
	if (lock->locked != _Q_LOCKED_COMBINER_VAL)
		BUG_ON(true);

	BUG_ON(from_cpuid == -1);

	if (max_idx < 0) {
		BUG_ON(true);
	}

#endif
	if (my_idx < max_idx) {
#ifdef KOMB_STATS
		this_cpu_inc(ooo_unlocks);
#endif

		lock->locked = _Q_UNLOCKED_OOO_VAL;
		return;
	}

	struct komb_node *curr_node = per_cpu_ptr(&komb_nodes[0], from_cpuid);

#ifdef WWJUMP
	struct komb_node *next_node = ptr->next_node_ptr;

	uint64_t counter = ptr->counter_val;

	if (next_node == NULL || next_node->next == NULL ||
	    check_irq_node(next_node) || check_irq_node(next_node->next) ||
	    counter >= komb_batch_size || need_resched()) {
		incoming_rsp_ptr = &(ptr->local_shadow_stack_ptr);
		ptr->curr_cs_cpu = -1;
		ptr->prev_cs_cpu = curr_node->cpuid;

	} else {
		ptr->curr_cs_cpu = next_node->cpuid;
		ptr->prev_cs_cpu = curr_node->cpuid;
		incoming_rsp_ptr = &(next_node->rsp);
		ptr->counter_val = counter + 1;
		print_debug("Jumping to the next waiter: %d\n",
			    next_node->cpuid);
	}
#else
	incoming_rsp_ptr = &(ptr->local_shadow_stack_ptr);
#endif
	/*
	 * Komb node still active here, because cpu (from_cpuid) still spinning.
	 */
	outgoing_rsp_ptr = &(curr_node->rsp);

#ifdef DEBUG_KOMB
	BUG_ON(incoming_rsp_ptr == NULL);
	BUG_ON(outgoing_rsp_ptr == NULL);
	BUG_ON(incoming_rsp_ptr == 0xdeadbeef);
	BUG_ON(outgoing_rsp_ptr == 0xdeadbeef);
#endif

#ifdef ENABLE_IRQS_CHECK
	curr_node->irqs_disabled = irqs_disabled();
#endif
	//local_irq_disable();
	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
	ptr = this_cpu_ptr(&local_shadow_stack);
	//local_irq_enable();
	if (ptr->irqs_disabled) {
		ptr->irqs_disabled = false;
		local_irq_disable();
	}
	LOCK_END_TIMING_PER_CPU(unlock_stack_switch);
	return;
}
EXPORT_SYMBOL_GPL(komb_spin_unlock);

__always_inline int komb_spin_trylock(struct qspinlock *lock)
{
	u32 val;

	// Qspinlock Fastpath

	val = atomic_cmpxchg_acquire(&lock->val, 0, _Q_LOCKED_VAL);
	if (val == 0)
		return true;

	return false;
}

void komb_spin_lock_nested(struct qspinlock *lock, int level)
{
	komb_spin_lock(lock);
}
EXPORT_SYMBOL_GPL(komb_spin_lock_nested);

void komb_assert_spin_locked(struct qspinlock *lock)
{
	BUG_ON(!komb_spin_is_locked(lock));
}

__always_inline int komb_spin_is_locked(struct qspinlock *lock)
{
	return atomic_read(&lock->val);
}
EXPORT_SYMBOL(komb_spin_is_locked);

__always_inline int komb_spin_is_contended(struct qspinlock *lock)
{
	return atomic_read(&lock->val) & ~_Q_LOCKED_MASK;
}

__always_inline int komb_spin_value_unlocked(struct qspinlock lock)
{
	return !atomic_read(&lock.val);
}
