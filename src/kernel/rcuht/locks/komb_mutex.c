// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022 Vishal Gupta, Kumar Kartikeya Dwivedi

#include <linux/module.h>
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/prefetch.h>
#include <linux/atomic.h>
#include <asm/byteorder.h>
#include <linux/vmalloc.h>
#include <linux/sched/stat.h>
#include <linux/sched/task.h>
#include <linux/sched.h>
#ifdef KERNEL_SYNCSTRESS
#include "mutex/komb_mutex.h"
#include "timing_stats.h"
#else
#include <linux/komb_mutex.h>
#include <linux/sched.h>
#include <linux/combiner.h>
#define LOCK_START_TIMING_PER_CPU(combiner_loop)
#define LOCK_END_TIMING_PER_CPU(combiner_loop)
#endif
#include <linux/topology.h>

//#define DSM_DEBUG 1
#ifdef DSM_DEBUG
#define print_debug(fmt, ...)                                                  \
	({                                                                     \
		printk(KERN_ALERT "[%d] [%d] komb (%s) lock(%px): " fmt,       \
		       smp_processor_id(), current->pid, __func__, lock,       \
		       ##__VA_ARGS__);                                         \
	})
#else
#define print_debug(fmt, ...)
#endif

//#define DEBUG_KOMB 1

#define smp_cond_load_relaxed_sleep(curr_node, ptr, cond_expr)                 \
	({                                                                     \
		typeof(ptr) __PTR = (ptr);                                     \
		__unqual_scalar_typeof(*ptr) VAL;                              \
		for (;;) {                                                     \
			VAL = READ_ONCE(*__PTR);                               \
			if (cond_expr)                                         \
				break;                                         \
			cpu_relax();                                           \
			if (need_resched()) {                                  \
				if (single_task_running())                     \
					schedule_out_curr_task();              \
				else {                                         \
					if (READ_ONCE(curr_node->completed) == \
					    KOMB_WAITER_UNPROCESSED)           \
						park_waiter(curr_node);        \
					else                                   \
						schedule_out_curr_task();      \
				}                                              \
			}                                                      \
		}                                                              \
		(typeof(*ptr))VAL;                                             \
	})

#ifndef smp_cond_load_relaxed_sched
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

#define UINT64_MAX 0xffffffffffffffffL

#ifdef LOCK_MEASURE_TIME
static DEFINE_PER_CPU_ALIGNED(uint64_t, combiner_loop);
#endif

#ifdef KOMB_STATS
static DEFINE_PER_CPU_ALIGNED(uint64_t, combiner_count);
static DEFINE_PER_CPU_ALIGNED(uint64_t, waiter_combined);
static DEFINE_PER_CPU_ALIGNED(uint64_t, ooo_combiner_count);
static DEFINE_PER_CPU_ALIGNED(uint64_t, ooo_waiter_combined);
static DEFINE_PER_CPU_ALIGNED(uint64_t, ooo_unlocks);
#endif

#define _Q_LOCKED_COMBINER_VAL 3
#define _Q_UNLOCKED_OOO_VAL 7 //Unlocked a lock out-of-order

#define KOMB_WAITER_UNPROCESSED 0
#define KOMB_WAITER_PARKED 1
#define KOMB_WAITER_PROCESSING 2
#define KOMB_WAITER_PROCESSED 4
static inline void schedule_out_curr_task(void)
{
	preempt_enable();
	schedule();
	preempt_disable();
}

static inline void park_waiter(struct komb_mutex_node *node)
{
	__set_current_state(TASK_INTERRUPTIBLE);

	if (cmpxchg(&node->completed, KOMB_WAITER_UNPROCESSED,
		    KOMB_WAITER_PARKED) != KOMB_WAITER_UNPROCESSED) {
		__set_current_state(TASK_RUNNING);
		return;
	}
	schedule_out_curr_task();
	__set_current_state(TASK_RUNNING);
}

static inline void wake_up_waiter(struct komb_mutex_node *node)
{
	u8 old_val = xchg(&node->completed, KOMB_WAITER_PROCESSING);

	if (old_val == KOMB_WAITER_PARKED) {
		struct task_struct *task = node->task_struct_ptr;
		get_task_struct(task);
		wake_up_process(task);
		put_task_struct(task);
	}
}

void komb_mutex_init(struct komb_mutex *lock)
{
	lock->tail = NULL;
	atomic_set(&lock->state, 0);
	lock->combiner_task = NULL;
}

static __always_inline void
clear_locked_set_completed(struct komb_mutex_node *node)
{
	WRITE_ONCE(node->completed, KOMB_WAITER_PROCESSED);
	WRITE_ONCE(node->locked, 0);
}

static __always_inline void set_locked(struct komb_mutex *lock)
{
	WRITE_ONCE(lock->locked, 1);
}

/*
 * lock -> rdi
 */
__attribute__((noipa)) noinline notrace static uint64_t
get_shadow_stack_ptr(struct komb_mutex *lock)
{
	return &current->komb_stack_curr_ptr;
}

__attribute__((noipa)) noinline notrace static struct komb_mutex_node *
get_komb_mutex_node(struct komb_mutex *lock)
{
	return ((struct komb_mutex_node *)(current->komb_mutex_node));
}

#ifdef NUMA_AWARE
__always_inline static void add_to_local_queue(struct komb_mutex_node *node)
{
	struct komb_mutex_node **head, **tail;

	head = (struct komb_mutex_node **)(&current->komb_local_queue_head);
	tail = (struct komb_mutex_node **)(&current->komb_local_queue_tail);

	if (*head == NULL) {
		*head = node;
		*tail = node;
	} else {
		(*tail)->next = node;
		*tail = node;
	}
}
#endif

__always_inline static struct komb_mutex_node *
get_next_node(struct komb_mutex_node *my_node)
{
#ifdef NUMA_AWARE
	struct komb_mutex_node *curr_node, *next_node;

	curr_node = my_node;
	next_node = curr_node->next;

	while (true) {
		if (next_node == NULL || next_node->next == NULL)
			goto next_node_null;

		if (next_node->socket_id == numa_node_id()) {
			void *rsp_ptr = (next_node->rsp);
			prefetchw(rsp_ptr);
			prefetchw(rsp_ptr + 64);
			prefetchw(rsp_ptr + 128);
			prefetchw(rsp_ptr + 192);
			prefetchw(rsp_ptr + 256);
			prefetchw(rsp_ptr + 320);

			prefetch(next_node->next);

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
execute_cs(struct komb_mutex *lock, struct komb_mutex_node *curr_node)
{
	void *incoming_rsp_ptr, *outgoing_rsp_ptr;
	struct komb_mutex_node *my_node, *next_node;

	WRITE_ONCE(current->komb_curr_waiter_task, curr_node->task_struct_ptr);
#ifdef DEBUG_KOMB
	BUG_ON(curr_node->cpuid == smp_processor_id());
#endif

	incoming_rsp_ptr = &(curr_node->rsp);
	outgoing_rsp_ptr = get_shadow_stack_ptr(lock);
#ifdef LOCK_MEASURE_TIME
	*this_cpu_ptr(&combiner_loop) = UINT64_MAX;
#endif
	/*
	 * Make the actual switch, the pushed return address is after this
	 * function call, which we will resume execution at using the switch
	 * in unlock.
	 */

	//local_irq_disable();
	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
	//local_irq_enable();

#ifdef DEBUG_KOMB
	BUG_ON(current->komb_stack_base_ptr - (current->komb_stack_curr_ptr) >
	       SIZE_OF_SHADOW_STACK);
#endif

	if (lock->locked == _Q_UNLOCKED_OOO_VAL) {
		print_debug("Combiner got control back OOO unlock\n");

#ifdef KOMB_STATS
		this_cpu_add(ooo_waiter_combined, current->counter_val);
		this_cpu_inc(ooo_combiner_count);
#endif

#ifdef DEBUG_KOMB
		BUG_ON(current->komb_curr_waiter_task == NULL);
#endif

		curr_node =
			((struct task_struct *)current->komb_curr_waiter_task)
				->komb_mutex_node;
		my_node = current->komb_mutex_node;

		if (curr_node) {
			//print_debug("OOO waking up %d\n", ptr->curr_cs_cpu);
			curr_node->rsp = my_node->rsp;
			wake_up_waiter(curr_node);
			clear_locked_set_completed(curr_node);
#ifdef DEBUG_KOMB
			BUG_ON(current->komb_prev_waiter_task != NULL);
#endif
		}
		current->komb_prev_waiter_task = NULL;
		current->komb_curr_waiter_task = NULL;
		lock->locked = _Q_LOCKED_COMBINER_VAL;

		next_node = NULL;
		if (current->komb_next_waiter_task)
			next_node = ((struct task_struct *)
					     current->komb_next_waiter_task)
					    ->komb_mutex_node;

		if (next_node && next_node->next) {
			execute_cs(lock, next_node);
		}
	}
}
#pragma GCC pop_options

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace static void
run_combiner(struct komb_mutex *lock, struct komb_mutex_node *curr_node)
{
#ifdef DEBUG_KOMB
	BUG_ON(curr_node == NULL);
#endif

#ifdef NUMA_AWARE
	struct komb_mutex_node **local_head, **local_tail;
#endif

	struct komb_mutex_node *next_node = curr_node->next;
	int counter = 0;

	if (next_node == NULL) {
		set_locked(lock);

		/* 
		 * Make this node spin on the locked variable and then it will 
		 * become the combiner.
		 */
	
		print_debug("mutex combiner only one in the queue. Passing lock to %d\n", curr_node->cpuid);
		wake_up_waiter(curr_node);
		WRITE_ONCE(curr_node->locked, 0);
		return;
	}

	WRITE_ONCE(lock->combiner_task, current);

	/* while (curr_node) {
		counter++;

		wake_up_waiter(curr_node);
		next_node = get_next_node(lock, curr_node);

		execute_cs(lock, curr_node);

		if (next_node == NULL || next_node->next == NULL ||
		    counter >= batch_size)
			break;

		curr_node = next_node;
	}*/
	current->counter_val = 0;

	print_debug("mutex Combiner %d giving control to %d\n", smp_processor_id(),
		    curr_node->cpuid);

	execute_cs(lock, curr_node);

	print_debug(
		"mutex Combiner got the control back: %d counter: %d last_waiter: %d\n",
		smp_processor_id(), current->counter_val,
		current->komb_curr_waiter_task);

#ifdef KOMB_STATS
	this_cpu_add(waiter_combined, current->counter_val);
	this_cpu_inc(combiner_count);
#endif

	if (current->komb_prev_waiter_task) {
		struct komb_mutex_node *prev_node =
			((struct task_struct *)current->komb_prev_waiter_task)
				->komb_mutex_node;
		wake_up_waiter(prev_node);
		clear_locked_set_completed(prev_node);
		current->komb_prev_waiter_task = NULL;
	}

	next_node = NULL;
	if (current->komb_next_waiter_task) {
		next_node =
			((struct task_struct *)current->komb_next_waiter_task)
				->komb_mutex_node;
		current->komb_next_waiter_task = NULL;
	}

	WRITE_ONCE(lock->combiner_task, NULL);

#ifdef NUMA_AWARE
	local_head =
		(struct komb_mutex_node **)(&current->komb_local_queue_head);
	local_tail =
		(struct komb_mutex_node **)(&current->komb_local_queue_tail);

	if (*local_head) {
		(*local_tail)->next = next_node;
		next_node = *local_head;
		*local_head = NULL;
		*local_tail = NULL;
	}
#endif

	set_locked(lock);

#ifdef DEBUG_KOMB
	BUG_ON(next_node == NULL);
#endif

	/*
	 * Make this node spin on the locked variabe and then it will 
	 * become the combiner.
	 */
	print_debug("mutex Passing lock to next node %d\n", next_node->cpuid);
	wake_up_waiter(next_node);
	WRITE_ONCE(next_node->locked, 0);
}
#pragma GCC pop_options

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace static int
__komb_mutex_lock_slowpath(struct komb_mutex *lock,
			   register struct komb_mutex_node *curr_node)
{
	register struct komb_mutex_node *prev, *next;
	u8 prev_locked_val;
	int j;
	struct komb_mutex *parent_lock;

#ifdef NUMA_AWARE
	struct komb_mutex_node *prev_local_queue_head;
	struct komb_mutex_node *prev_local_queue_tail;
#endif

	prev = xchg(&lock->tail, curr_node);
	next = NULL;

	if (prev) {
		WRITE_ONCE(prev->next, curr_node);

		print_debug("Mutex waiter going to spin\n");
		smp_cond_load_relaxed_sleep(curr_node, &curr_node->locked,
					    VAL == 0);

		print_debug("Mutex spin completed\n");
		if (READ_ONCE(curr_node->completed) == KOMB_WAITER_PROCESSED) {
			print_debug("Mutex CS processed\n");
			for (j = 7; j >= 0; j--)
				if (current->komb_lock_addr[j])
					break;

			if (j >= 0) {
				parent_lock = current->komb_lock_addr[j];
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
			return 0;
		}
	}

	for (;;) {
		while (READ_ONCE(lock->locked)) {
			cpu_relax();
			if (need_resched())
				schedule_out_curr_task();
		}

		if (cmpxchg(&lock->locked, 0, 1) == 0)
			break;
	}

	print_debug("Mutex lock owner\n");

	if (cmpxchg(&lock->tail, curr_node, NULL) == curr_node)
		goto release;

	while (!next) {
		next = READ_ONCE(curr_node->next);

		cpu_relax();
		if (need_resched())
			schedule_out_curr_task();
	}

	//Head of the queue. Run Combiner.

	struct task_struct *prev_curr_waiter_task =
		current->komb_curr_waiter_task;
	current->komb_curr_waiter_task = NULL;

#ifdef DEBUG_KOMB
	BUG_ON(current->komb_prev_waiter_task != NULL);
#endif

	prev_locked_val = lock->locked;
#ifdef DEBUG_KOMB
	BUG_ON(prev_locked_val >= _Q_LOCKED_COMBINER_VAL);
#endif

	lock->locked = _Q_LOCKED_COMBINER_VAL;

	struct task_struct *prev_task_struct_ptr = curr_node->task_struct_ptr;
	uint64_t prev_rsp = curr_node->rsp;
	curr_node->rsp = NULL;
	uint64_t prev_counter_val = current->counter_val;
	current->counter_val = 0;

	struct task_struct *prev_next_waiter_task =
		current->komb_next_waiter_task;
	current->komb_next_waiter_task = NULL;

	uint64_t prev_stack_curr_ptr = current->komb_stack_curr_ptr;

#ifdef NUMA_AWARE
	prev_local_queue_head =
		(struct komb_mutex_node *)current->komb_local_queue_head;
	prev_local_queue_tail =
		(struct komb_mutex_node *)current->komb_local_queue_tail;

	current->komb_local_queue_head = NULL;
	current->komb_local_queue_tail = NULL;
#endif

	j = 7;
	for (j = 7; j >= 0; j--)
		if (current->komb_lock_addr[j])
			break;
	j += 1;
#ifdef DEBUG_KOMB
	BUG_ON(j >= 8 || j < 0);
#endif
	current->komb_lock_addr[j] = lock;

	run_combiner(lock, next);
#ifdef DEBUG_KOMB
	BUG_ON(current->komb_lock_addr[j] != lock);
#endif

	current->komb_lock_addr[j] = NULL;

	current->komb_next_waiter_task = prev_next_waiter_task;
	current->counter_val = prev_counter_val;

#ifdef NUMA_AWARE
	current->komb_local_queue_head = prev_local_queue_head;
	current->komb_local_queue_tail = prev_local_queue_tail;
#endif

	current->komb_stack_curr_ptr = prev_stack_curr_ptr;
	current->komb_curr_waiter_task = prev_curr_waiter_task;
	curr_node->rsp = prev_rsp;
	curr_node->task_struct_ptr = prev_task_struct_ptr;

	if (lock->locked == _Q_UNLOCKED_OOO_VAL) {
		if (prev_curr_waiter_task) {
			//print_debug("Waking up %d\n", prev_cs_cpu);
			wake_up_waiter(
				((struct task_struct *)prev_curr_waiter_task)
					->komb_mutex_node);
			clear_locked_set_completed(
				((struct task_struct *)prev_curr_waiter_task)
					->komb_mutex_node);
		}
		current->komb_curr_waiter_task = NULL;
	}
	lock->locked = prev_locked_val;

release:
	return 0;
}
#pragma GCC pop_options

__attribute__((noipa)) noinline notrace static int
komb_mutex_lock_slowpath(struct komb_mutex *lock)
{
	struct komb_mutex_node *curr_node = get_komb_mutex_node(lock);

	curr_node->locked = true;
	curr_node->completed = KOMB_WAITER_UNPROCESSED;
	curr_node->next = NULL;
	//curr_node->socket_id = numa_node_id();
	curr_node->cpuid = smp_processor_id();
	curr_node->task_struct_ptr = current;
	curr_node->lock = lock;

	return __komb_mutex_lock_slowpath(lock, curr_node);
}

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace void
__komb_mutex_lock(struct komb_mutex *lock)
{
	register int ret_val;
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
		     : "i"(get_komb_mutex_node),
		       "i"(offsetof(struct komb_mutex_node, rsp))
		     : "memory");
	asm volatile("callq %P0\n"
		     "movq (%%rax), %%rsp\n"
		     "pushq %%rdi\n"
		     :
		     : "i"(get_shadow_stack_ptr)
		     : "memory");
	//local_irq_enable();

	ret_val = komb_mutex_lock_slowpath(lock);

	//local_irq_disable();
	if (ret_val) {
		asm volatile("popq %%rdi\n"
			     "callq %P0\n"
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
		asm volatile("popq %%rdi\n"
			     "callq %P0\n"
			     "movq %%rsp, (%%rax)\n"
			     :
			     : "i"(get_shadow_stack_ptr)
			     : "memory");
		asm volatile("callq %P0\n"
			     "movq %c1(%%rax), %%rsp\n"
			     :
			     : "i"(get_komb_mutex_node),
			       "i"(offsetof(struct komb_mutex_node, rsp))
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
komb_mutex_lock(struct komb_mutex *lock)
{
	int ret;

	ret = cmpxchg(&lock->locked, 0, 1);
	if (likely(ret == 0)) {
		/*if (lock->tail != NULL)
			print_debug("Lock stealing\n");*/
		return;
	}

	preempt_disable();
	struct komb_mutex_node *curr_node = get_komb_mutex_node(lock);
	curr_node->socket_id = numa_node_id();

	__komb_mutex_lock(lock);

	if (current->komb_curr_waiter_task) {
		struct komb_mutex_node *curr_node =
			((struct task_struct *)current->komb_curr_waiter_task)
				->komb_mutex_node;

		if ((struct komb_mutex *)curr_node->lock == lock) {
#ifdef DEBUG_KOMB
			BUG_ON(lock->locked != _Q_LOCKED_COMBINER_VAL);
#endif
			struct komb_mutex_node *next_node =
				get_next_node(curr_node);
			if (next_node == NULL)
				current->komb_next_waiter_task = NULL;
			else
				current->komb_next_waiter_task =
					next_node->task_struct_ptr;
		}

		wake_up_waiter(curr_node);

		if (current->komb_prev_waiter_task) {
			struct komb_mutex_node *prev_node =
				((struct task_struct *)
					 current->komb_prev_waiter_task)
					->komb_mutex_node;

#ifdef DEBUG_KOMB
			BUG_ON(prev_node->lock != lock);
#endif
			print_debug("Waking up prev waiter: %d\n", prev_node->cpuid);
			wake_up_waiter(prev_node);
			clear_locked_set_completed(prev_node);
			current->komb_prev_waiter_task = NULL;
		}
	}

	preempt_enable();
}

__attribute__((noipa)) noinline notrace void
komb_mutex_lock_rwsem_writer(struct komb_mutex *lock)
{
	int ret;

	ret = cmpxchg(&lock->locked, 0, 1);
	if (likely(ret == 0)) {
		/*if (lock->tail != NULL)
			print_debug("Lock stealing\n");*/
		return;
	}

	preempt_disable();
	struct komb_mutex_node *curr_node = get_komb_mutex_node(lock);
	curr_node->socket_id = 255; //Random socket id  
	__komb_mutex_lock(lock);



	BUG_ON(current->komb_curr_waiter_task != NULL);

	preempt_enable();
}



int __must_check komb_mutex_lock_interruptible(struct komb_mutex *lock)
{
	return 0;
}

int __must_check komb_mutex_lock_killable(struct komb_mutex *lock)
{
	return 0;
}

void komb_mutex_lock_io(struct komb_mutex *lock)
{
}

int komb_mutex_trylock(struct komb_mutex *lock)
{
	if (!lock->locked && cmpxchg(&lock->locked, 0, 1) == 0)
		return 1;

	return 0;
}

__attribute__((noipa)) noinline notrace void
komb_mutex_unlock(struct komb_mutex *lock)
{
	void *incoming_rsp_ptr, *outgoing_rsp_ptr;
	struct task_struct *curr_task;
	struct komb_mutex_node *curr_node;

	int j, max_idx, my_idx;

	uint64_t temp_lock_addr;

	j = 0;
	max_idx = -1;
	my_idx = -1;

	for (j = 0; j < 8; j++) {
		temp_lock_addr = current->komb_lock_addr[j];
		if (temp_lock_addr)
			max_idx = j;
		if (temp_lock_addr == lock)
			my_idx = j;
		if (temp_lock_addr == NULL)
			break;
	}

	if (my_idx == -1) {
		if (lock->locked == _Q_LOCKED_VAL)
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

	BUG_ON(current->komb_curr_waiter_task == NULL);

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

	curr_node = ((struct task_struct *)current->komb_curr_waiter_task)
			    ->komb_mutex_node;

	struct komb_mutex_node *next_node = NULL;
	if (current->komb_next_waiter_task)
		next_node =
			((struct task_struct *)current->komb_next_waiter_task)
				->komb_mutex_node;

	uint64_t counter = current->counter_val;

#ifdef DEBUG_KOMB
	BUG_ON(lock->combiner_task != current);
#endif

	if (next_node == NULL || next_node->next == NULL ||
	    counter >= komb_batch_size) {
		incoming_rsp_ptr = &(current->komb_stack_curr_ptr);
		current->komb_prev_waiter_task = current->komb_curr_waiter_task;
		current->komb_curr_waiter_task = NULL;

	} else {
		current->komb_prev_waiter_task = current->komb_curr_waiter_task;
		current->komb_curr_waiter_task = current->komb_next_waiter_task;
		incoming_rsp_ptr = &(next_node->rsp);
		current->counter_val = counter + 1;
		print_debug("Jumping to the next waiter: %d\n",
			    next_node->cpuid);
	}

	/*
	 * Komb node still active here, because cpu (from_cpuid) still spinning.
	 */
	outgoing_rsp_ptr = &(curr_node->rsp);

	preempt_disable();
	//local_irq_disable();
	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
	//local_irq_enable();
	preempt_enable();
	return;
}
