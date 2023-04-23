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
#include "rwsem/komb_rwsem.h"
#include "timing_stats.h"
#else
#include <linux/komb_rwsem.h>
#include <linux/sched.h>
#include <linux/combiner.h>
#define LOCK_DEFINE_TIMING_VAR(var)
#define LOCK_START_TIMING_PER_CPU(var)
#define LOCK_END_TIMING_PER_CPU(var)
#define LOCK_START_TIMING(var_t, var)
#define LOCK_END_TIMING(var_t, var)
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

#define READ_STATE 0
#define WRITE_STATE 1

#define KOMB_WAITER_UNPROCESSED 0
#define KOMB_WAITER_PARKED 1
#define KOMB_WAITER_PROCESSING 2
#define KOMB_WAITER_PROCESSED 4
#define KOMB_WAITER_WAKER 8

#define KOMB_WAKER_COUNT_SHIFT 16
#define KOMB_WAKER_COUNT (1U << KOMB_WAKER_COUNT_SHIFT)

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
			if (need_resched()) {preempt_enable(); schedule(); preempt_disable();\
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

#define atomic_long_cond_read_acquire_sched(v, c)                              \
	smp_cond_load_acquire_sched(&(v)->counter, (c))

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

#ifdef BRAVO

uint64_t **global_vr_table;

static DEFINE_PER_CPU(u32, check_bias);

static inline uint32_t mix32a(uint32_t v)
{
    static const uint32_t mix32ka = 0x9abe94e3 ;
    v = (v ^ (v >> 16)) * mix32ka ;
    v = (v ^ (v >> 16)) * mix32ka ;
    return v;
}

static inline uint32_t hash(uint64_t addr) {
	return mix32a((uint64_t)current ^ addr) % NUM_SLOT;
	//return (smp_processor_id()+128) % NUM_SLOT;
}

static inline void wait_for_visible_readers(struct komb_rwsem *lock)
{
	if (READ_ONCE(lock->rbias)) {
		int i, j;
		unsigned long start, now;
		uint64_t **vr_table;

		smp_mb();
		WRITE_ONCE(lock->rbias, 0);


		vr_table = global_vr_table;
		start = rdtsc();
		for (i = 0; i < NUM_SLOT; i += 8) {
			smp_cond_load_relaxed_sched(&vr_table[V(i)], (VAL == 0));
		}
		now = rdtsc();
		lock->inhibit_until = now + ((now - start) * MULTIPLIER);
	}
}
#endif //BRAVO

/*
 * lock -> rdi
 */
__attribute__((noipa)) noinline notrace static uint64_t
get_shadow_stack_ptr(struct komb_rwsem *lock)
{
	return &current->komb_stack_curr_ptr;
}

__attribute__((noipa)) noinline notrace static struct komb_mutex_node *
get_komb_mutex_node(struct komb_rwsem *lock)
{
	return ((struct komb_mutex_node *)(current->komb_mutex_node));
}

static __always_inline void set_locked(struct komb_rwsem *lock)
{
	WRITE_ONCE(lock->wlocked, _KOMB_RWSEM_W_LOCKED);
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
		(*tail)->next_rwsem = node;
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
	next_node = curr_node->next_rwsem;

	while (true) {
		if (next_node == NULL || next_node->next_rwsem == NULL)
			goto next_node_null;

		prefetch(next_node->next_rwsem);

		if (next_node->socket_id == numa_node_id()) {
			void *rsp_ptr = (next_node->rsp);
			prefetchw(rsp_ptr);
			prefetchw(rsp_ptr + 64);
			prefetchw(rsp_ptr + 128);
			prefetchw(rsp_ptr + 192);
			prefetchw(rsp_ptr + 256);
			prefetchw(rsp_ptr + 320);

			return next_node;
		}

		add_to_local_queue(next_node);
		curr_node = next_node;
		next_node = curr_node->next_rwsem;
	}

next_node_null:
	return next_node;
#else
	return my_node->next_rwsem;
#endif
}

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

static __always_inline void
clear_locked_set_completed(struct komb_mutex_node *node)
{
	WRITE_ONCE(node->completed, KOMB_WAITER_PROCESSED);
	WRITE_ONCE(node->locked, 0);
}

static inline void komb_read_lock_slowpath(struct komb_rwsem *lock)
{
	print_debug("Reader waiting for spinlock\n");
	aqs_spin_lock(&lock->reader_wait_lock);
	atomic_long_add_return_acquire(_KOMB_RWSEM_R_BIAS, &lock->cnts);
	print_debug("Reader slowpath got wait lock, waiting for writer to go away\n");
	atomic_long_cond_read_acquire(&lock->cnts, !(VAL & _KOMB_RWSEM_W_WMASK));
	print_debug("Reader slowpath got the lock\n");
	aqs_spin_unlock(&lock->reader_wait_lock);	
	return;

//	struct komb_mutex_node *curr_node = get_komb_mutex_node(lock);
//
//	curr_node->locked = true;
//	curr_node->completed = KOMB_WAITER_UNPROCESSED;
//	curr_node->next = NULL;
//	curr_node->tail = NULL;
//	curr_node->socket_id = numa_node_id();
//	curr_node->cpuid = smp_processor_id();
//	curr_node->task_struct_ptr = current;
//	curr_node->lock = lock;
//
//	struct komb_mutex_node *prev, *next;
//
//	prev = xchg(&lock->reader_tail, curr_node);
//
//	if (prev) {
//		WRITE_ONCE(prev->next, curr_node);
//
//		smp_cond_load_relaxed_sleep(curr_node, &curr_node->locked,
//					    VAL == 0);
//
//		if (READ_ONCE(curr_node->completed) == KOMB_WAITER_PROCESSED) {
//			atomic_long_add_return_acquire(_KOMB_RWSEM_R_BIAS,
//						       &lock->cnts);
//			return;
//		}
//	}
//
//	struct komb_mutex_node *temp_curr_node = curr_node;
//	struct komb_mutex_node *temp_next_node =
//		READ_ONCE(temp_curr_node->next);
//
//	int node_ptr_index = 0;
//	struct komb_mutex_node *node_ptr[16];
//
//	for (;;) {
//		u64 cnts = atomic_long_add_return_acquire(_KOMB_RWSEM_R_BIAS,
//							  &lock->cnts);
//
//		if ((cnts & _KOMB_RWSEM_W_WMASK) == 0)
//			break;
//
//		atomic_long_sub_return_release(_KOMB_RWSEM_R_BIAS, &lock->cnts);
//		atomic_long_cond_read_acquire(&lock->cnts, VAL == 0);
//
//		/*for (;;) {
//			if (atomic_long_read(&lock->cnts) == 0)
//				break;
//
//			temp_next_node = READ_ONCE(temp_curr_node->next);
//			if (temp_next_node) {
//				if (node_ptr_index < 16)
//					node_ptr[node_ptr_index++] =
//						temp_curr_node;
//				temp_curr_node = temp_next_node;
//				temp_next_node =
//					READ_ONCE(temp_curr_node->next);
//			}
//
//			cpu_relax();
//
//			if (need_resched())
//				schedule_out_curr_task();
//		}*/
//	}
//
//	if (cmpxchg(&lock->reader_tail, curr_node, NULL) == curr_node)
//		return;
//
//	LOCK_DEFINE_TIMING_VAR(reader_owner);
//	LOCK_START_TIMING_DISABLE(reader_owner_t, reader_owner);
//
//	int i = 0;
//	//if (node_ptr_index == 0) {
//	next = READ_ONCE(curr_node->next);
//
//	while (!next) {
//		next = READ_ONCE(curr_node->next);
//
//		cpu_relax();
//		if (need_resched()) {
//			preempt_enable();
//			schedule();
//			preempt_disable();
//		}
//	}
//
//	curr_node = next;
//	next = curr_node->next;
//	/*} else {
//		for (i = 1; i < (node_ptr_index - 1); i++) {
//			wake_up_waiter(node_ptr[i]);
//			clear_locked_set_completed(node_ptr[i]);
//		}
//
//		curr_node = node_ptr[node_ptr_index - 1];
//		next = READ_ONCE(curr_node->next);
//		node_ptr_index = 0;
//	}*/
//
//	while (next) {
//		prefetchw(next);
//		wake_up_waiter(curr_node);
//		clear_locked_set_completed(curr_node);
//
//		/*node_ptr[node_ptr_index++] = curr_node;
//
//		if (node_ptr_index == 16) {
//			for (i = 0; i < node_ptr_index; i++) {
//				wake_up_waiter(node_ptr[i]);
//				clear_locked_set_completed(node_ptr[i]);
//			}
//			node_ptr_index = 0;
//		}*/
//
//		curr_node = next;
//		next = curr_node->next;
//	}
//
//	/*for (i = 0; i < node_ptr_index; i++) {
//		wake_up_waiter(node_ptr[i]);
//		clear_locked_set_completed(node_ptr[i]);
//	}*/
//
//	wake_up_waiter(curr_node);
//	WRITE_ONCE(curr_node->locked, false);
//
//	LOCK_END_TIMING_DISABLE(reader_owner_t, reader_owner);
}

void komb_rwsem_down_read(struct komb_rwsem *lock)
{
#ifdef BRAVO
	if(READ_ONCE(lock->rbias)) {
		uint64_t **slot = NULL;
		uint64_t new_val = (uint64_t)&current->pid;
		u32 id = hash(new_val);
		//printk(KERN_ALERT "[%d] down_read id:%d vid:%d", smp_processor_id(), id, V(id));
		slot = &global_vr_table[V(id)];

		if(cmpxchg(slot, 0, new_val) == 0) {
			if(READ_ONCE(lock->rbias))
					return;
			(void)xchg(slot, NULL);
		}
	}
#endif

	u64 cnts;

	cnts = atomic_long_add_return_acquire(_KOMB_RWSEM_R_BIAS, &lock->cnts);

	if (likely(!(cnts & _KOMB_RWSEM_W_WMASK)))
	{
		//print_debug("Reader got the lock on fastpath\n");
		return;
	}

	(void)atomic_long_sub_return_release(_KOMB_RWSEM_R_BIAS, &lock->cnts);

	//preempt_disable();
	komb_read_lock_slowpath(lock);

#ifdef BRAVO
		if (((this_cpu_inc_return(check_bias) % CHECK_FOR_BIAS) == 0) &&
		    (!READ_ONCE(lock->rbias) && rdtsc() >= lock->inhibit_until))
			WRITE_ONCE(lock->rbias, 1);
#endif

	//preempt_enable();
}

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace static void
execute_cs(struct komb_rwsem *lock, struct komb_mutex_node *curr_node)
{
	void *incoming_rsp_ptr, *outgoing_rsp_ptr;

	WRITE_ONCE(current->komb_curr_waiter_task, curr_node->task_struct_ptr);

	print_debug("curr_node: %d task_struct_ptr: %px current: %px\n", curr_node->cpuid, current->komb_curr_waiter_task, current);

	struct komb_mutex_node *next_node, *my_node;
#ifdef DEBUG_KOMB
	BUG_ON(curr_node->cpuid == smp_processor_id());
#endif

	/*if (curr_node->next && curr_node->next->next) {
		prefetch(curr_node->next->next);
		prefetchw(curr_node->next->next->rsp);
		prefetchw(curr_node->next->next->rsp + 64);
	}*/

	incoming_rsp_ptr = &(curr_node->rsp);
	outgoing_rsp_ptr = &current->komb_stack_curr_ptr;
#ifdef LOCK_MEASURE_TIME
	*this_cpu_ptr(&combiner_loop) = UINT64_MAX;
#endif

	//outgoing_rsp_ptr = get_shadow_stack_ptr(lock);

	// LOCK_DEFINE_TIMING_VAR(execute_cs);
	LOCK_START_TIMING_DISABLE(execute_cs_t, execute_cs);

	/*
	 * Switch the stack pointer.
	 * IRQs enabled in the komb_context_switch function.
	 */
	//local_irq_disable();
	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
	//local_irq_enable();

	LOCK_END_TIMING_DISABLE(execute_cs_t, execute_cs);
#ifdef DEBUG_KOMB
	BUG_ON(current->komb_stack_base_ptr - (current->komb_stack_curr_ptr) >
	       SIZE_OF_SHADOW_STACK);
#endif

	if (lock->wlocked == _KOMB_RWSEM_W_OOO) {
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
			print_debug("OOO waking up \n");
			curr_node->rsp = my_node->rsp;
			wake_up_waiter(curr_node);
			clear_locked_set_completed(curr_node);
#ifdef DEBUG_KOMB
			BUG_ON(current->komb_prev_waiter_task != NULL);
#endif
		}
		current->komb_prev_waiter_task = NULL;
		current->komb_curr_waiter_task = NULL;
		lock->wlocked = _KOMB_RWSEM_W_COMBINER;

		next_node = NULL;
		if (current->komb_next_waiter_task)
			next_node = ((struct task_struct *)
					     current->komb_next_waiter_task)
					    ->komb_mutex_node;

		if (next_node && next_node->next_rwsem) {
			execute_cs(lock, next_node);
		}
	}
}
#pragma GCC pop_options

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace static void
run_combiner(struct komb_rwsem *lock, struct komb_mutex_node *curr_node)
{
#ifdef NUMA_AWARE
	struct komb_mutex_node **local_head, **local_tail;
#endif

	struct komb_mutex_node *next_node = curr_node->next_rwsem, *waker = curr_node;
	int counter = 0;

	if (next_node == NULL) {
		set_locked(lock);

		/* 
		 * Make this node spin on the locked variable and then it will 
		 * become the combiner.
		 */
		print_debug("Passing writer owner to %d\n", curr_node->cpuid);
		wake_up_waiter(curr_node);
		WRITE_ONCE(curr_node->locked, 0);
		return;
	}

	/*while (curr_node) {
		LOCK_DEFINE_TIMING_VAR(combiner_loop);
		LOCK_START_TIMING(combiner_loop_t, combiner_loop);

		counter++;

		next_node = get_next_node(lock, curr_node);

		curr_node->next = next_node;
		wake_up_waiter(curr_node);

		execute_cs(lock, curr_node);

		if (counter == 1) {
			BUG_ON(waker != curr_node);
			wake_up_waiter(waker);
			waker->completed = KOMB_WAITER_WAKER;
			waker->locked = 0;
		} else {
			atomic_long_add_return_acquire(KOMB_WAKER_COUNT,
						       &waker->cnts);
		}

	clear_locked_set_completed(curr_node);

	if (next_node == NULL || next_node->next == NULL ||
	    counter >= batch_size)
		break;

	curr_node = next_node;

	LOCK_END_TIMING(combiner_loop_t, combiner_loop);
	}
#ifdef LOCK_MEASURE_TIME
	*this_cpu_ptr(&combiner_loop) = UINT64_MAX;
#endif

*/
	current->counter_val = 0;

	print_debug("Combiner %d giving control to %d next_node: %d\n", smp_processor_id(),
		    curr_node->cpuid, next_node->cpuid);

	execute_cs(lock, curr_node);

	print_debug(
		"Combiner got the control back: %d counter: %d prev_waiter: %px next_waiter: %px\n",
		smp_processor_id(), current->counter_val,
		current->komb_prev_waiter_task, current->komb_next_waiter_task);

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
		print_debug("Combiner writer waking up %d\n", prev_node->cpuid);
		current->komb_prev_waiter_task = NULL;
	}

	next_node = NULL;
	if (current->komb_next_waiter_task) {
		next_node =
			((struct task_struct *)current->komb_next_waiter_task)
				->komb_mutex_node;
		current->komb_next_waiter_task = NULL;
	}

#ifdef NUMA_AWARE
	local_head =
		(struct komb_mutex_node **)(&current->komb_local_queue_head);
	local_tail =
		(struct komb_mutex_node **)(&current->komb_local_queue_tail);

	if (*local_head) {
		(*local_tail)->next_rwsem = next_node;
		next_node = *local_head;
		*local_head = NULL;
		*local_tail = NULL;
	}
#endif

	set_locked(lock);

#ifdef DEBUG_KOMB
	BUG_ON(next_node == NULL);
#endif

	current->komb_curr_waiter_task = NULL;
	/*
	 * Make this node spin on the locked variabe and then it will 
	 * become the combiner.
	 */
	print_debug("Writer passing owner to %d\n", next_node->cpuid);
	wake_up_waiter(next_node);
	WRITE_ONCE(next_node->locked, 0);

	/*if (waker->completed == KOMB_WAITER_WAKER) {
		while (true) {
			u64 val = atomic_long_read(&waker->val);
			if ((val >> KOMB_WAKER_COUNT_SHIFT) == 0)
				break;

			cpu_relax();
			if (need_resched())
				schedule_out_curr_task();
		}
		waker->completed = KOMB_WAITER_PROCESSED;
	}*/
}
#pragma GCC pop_options

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace static int
__komb_write_lock_slowpath(register struct komb_rwsem *lock)
{
#ifdef NUMA_AWARE
	struct komb_mutex_node *prev_local_queue_head;
	struct komb_mutex_node *prev_local_queue_tail;
#endif

	register struct komb_mutex_node *prev, *next,
		*curr_node = get_komb_mutex_node(lock);
	u8 prev_locked_val;
	int j;

	prev = xchg(&lock->writer_tail, curr_node);
	next = NULL;

	if (prev) {
		WRITE_ONCE(prev->next_rwsem, curr_node);

		print_debug("Writer waiting prev_node: %d\n", prev->cpuid);

		smp_cond_load_relaxed_sleep(curr_node, &curr_node->locked,
					    VAL == 0);

		print_debug("Writer waiting done\n");

		/*struct komb_mutex_node *temp_node = curr_node;
		struct komb_mutex_node *temp_next_node =
			READ_ONCE(temp_node->next_rwsem);*/

		/*while (READ_ONCE(curr_node->completed) == KOMB_WAITER_WAKER) {
			u64 cnts = atomic_long_read(&curr_node->cnts);
			if (cnts >> KOMB_WAKER_COUNT_SHIFT) {
				temp_node = READ_ONCE(temp_next_node);
				temp_next_node = READ_ONCE(temp_node->next);

				wake_up_waiter(temp_node);
				clear_locked_set_completed(temp_node);

				atomic_long_sub_return_release(
					KOMB_WAKER_COUNT, &curr_node->cnts);
			}
			cpu_relax();
			if (need_resched())
				schedule_out_curr_task();
		}*/
		if (READ_ONCE(curr_node->completed) == KOMB_WAITER_PROCESSED) {
			print_debug("Writer CS combined\n");
			for (j = 7; j >= 0; j--)
				if (current->komb_lock_addr[j])
					break;

			if (j >= 0) {
				struct komb_rwsem *parent_lock =
					current->komb_lock_addr[j];
#ifdef DEBUG_KOMB
				BUG_ON(parent_lock == lock);
#endif
				if (parent_lock->wlocked == _KOMB_RWSEM_W_OOO) {
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

	/*for (;;) {
		atomic_long_cond_read_acquire_sched(&lock->cnts, VAL == 0);
		if (atomic_long_cmpxchg_relaxed(&lock->cnts, 0,
						_KOMB_RWSEM_W_LOCKED) == 0)
			break;
	}*/

	print_debug("Writer owner on slowpath\n");
	//komb_mutex_lock_rwsem_writer(&lock->reader_wait_lock);
	aqs_spin_lock(&lock->reader_wait_lock);

	print_debug("Writer got the mutex lock. waiting for pending readers\n");

	if (!atomic_long_read(&lock->cnts) &&
	    (atomic_long_cmpxchg_relaxed(&lock->cnts, 0, _KOMB_RWSEM_W_LOCKED) == 0))
	{
		print_debug("No pending readers\n");
		goto unlock;
	}

	atomic_long_add_return_acquire(_KOMB_RWSEM_W_WAITING, &lock->cnts);
	print_debug("Writer set the pending bit\n");
	do {
		atomic_long_cond_read_acquire(&lock->cnts, VAL == _KOMB_RWSEM_W_WAITING);
	} while (atomic_long_cmpxchg_relaxed(&lock->cnts, _KOMB_RWSEM_W_WAITING,
					     _KOMB_RWSEM_W_LOCKED) != _KOMB_RWSEM_W_WAITING);
unlock:
	print_debug("Writer got the lock slowpath\n");
	aqs_spin_unlock(&lock->reader_wait_lock);


#ifdef BRAVO
	wait_for_visible_readers(lock);
#endif

	if (cmpxchg(&lock->writer_tail, curr_node, NULL) == curr_node)
	{
		print_debug("Writer only one in the queue\n");
		goto release;
	}

	while (!next) {
		next = READ_ONCE(curr_node->next_rwsem);

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

	prev_locked_val = lock->wlocked;
#ifdef DEBUG_KOMB
	BUG_ON(prev_locked_val == _KOMB_RWSEM_W_COMBINER);
#endif

	lock->wlocked = _KOMB_RWSEM_W_COMBINER;

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

	if (lock->wlocked == _KOMB_RWSEM_W_OOO) {
		if (prev_curr_waiter_task) {
			print_debug("Waking up \n");
			wake_up_waiter(
				((struct task_struct *)prev_curr_waiter_task)
					->komb_mutex_node);
			clear_locked_set_completed(
				((struct task_struct *)prev_curr_waiter_task)
					->komb_mutex_node);
		}
		current->komb_curr_waiter_task = NULL;
	}

	print_debug("Writer done with combining\n");
	WRITE_ONCE(lock->wlocked, prev_locked_val);

release:
	return 0;
}
#pragma GCC pop_options

__attribute__((noipa)) noinline notrace static int
komb_write_lock_slowpath(struct komb_rwsem *lock)
{
	struct komb_mutex_node *curr_node = get_komb_mutex_node(lock);

	curr_node->locked = true;
	curr_node->completed = KOMB_WAITER_UNPROCESSED;
	curr_node->next_rwsem = NULL;
	curr_node->tail = NULL;
	curr_node->socket_id = numa_node_id();
	curr_node->cpuid = smp_processor_id();
	curr_node->task_struct_ptr = current;
	curr_node->lock = lock;

	smp_wmb();

	return __komb_write_lock_slowpath(lock);
}

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace void
__komb_write_stack_switch(struct komb_rwsem *lock)
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

	ret_val = komb_write_lock_slowpath(lock);

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

/*
 * lock for writing
 */
void komb_rwsem_down_write(struct komb_rwsem *lock)
{
	u64 val, cnt;
	val = atomic_long_cmpxchg_acquire(&lock->cnts, 0, _KOMB_RWSEM_W_LOCKED);
	if (val == 0)
	{
		//print_debug("Writer got the lock on fastpath\n");
#ifdef BRAVO
		wait_for_visible_readers(lock);
#endif
		return;
	}

#if WRITE_BOUNDED_OPPORTUNISTIC_SPIN
	if ((val & _KOMB_RWSEM_W_WMASK) == _KOMB_RWSEM_W_LOCKED) {
		cnt = 512;
		val = atomic_long_cond_read_acquire_sched(
			&lock->cnts, (VAL == 0) || !(cnt--));
	}

	if (val == 0) {
		val = atomic_long_cmpxchg_acquire(&lock->cnts, 0,
						  _KOMB_RWSEM_W_LOCKED);
		if (val == 0)
		{
#ifdef BRAVO
			wait_for_visible_readers(lock);
#endif
			return;
		}
	}
#endif

	preempt_disable();
	print_debug("Writer going on slowpath\n");
	__komb_write_stack_switch(lock);

#ifdef DEBUG_KOMB
	if (lock->wlocked == _KOMB_RWSEM_W_COMBINER)
	{
		if(current->komb_curr_waiter_task == NULL)
			print_debug("Combiner check current: %px\n", current);
		BUG_ON(READ_ONCE(current->komb_curr_waiter_task) == NULL);
	}
#endif

	if (READ_ONCE(current->komb_curr_waiter_task)) {
		struct komb_mutex_node *curr_node =
			((struct task_struct *)current->komb_curr_waiter_task)
				->komb_mutex_node;

		print_debug("komb-curr-waiter_tsk\n");

		if ((struct komb_rwsem *)curr_node->lock == lock) {
#ifdef DEBUG_KOMB
			BUG_ON(lock->wlocked != _KOMB_RWSEM_W_COMBINER);
#endif
			struct komb_mutex_node *next_node =
				get_next_node(curr_node);
			print_debug("get_next_node called\n");
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
			print_debug("Waking up prev waiter: %d\n",
				    prev_node->cpuid);
			wake_up_waiter(prev_node);
			clear_locked_set_completed(prev_node);
			current->komb_prev_waiter_task = NULL;
		}
	}
	preempt_enable();
}

/*
 * release a read lock
 */
void komb_rwsem_up_read(struct komb_rwsem *lock)
{
#ifdef BRAVO
	uint64_t **slot = NULL;
	uint64_t new_val = (uint64_t)&current->pid;
	u32 id = hash(new_val);
	//printk(KERN_ALERT "[%d] up_read id:%d vid:%d", smp_processor_id(), id, V(id));

	slot = &global_vr_table[V(id)];
	if (cmpxchg(slot, new_val, 0) == new_val)
		return;
#endif

	//print_debug("read unlock\n");
	atomic_long_sub_return_release(_KOMB_RWSEM_R_BIAS, &lock->cnts);
}

/*
 * release a write lock
 */
__attribute__((noipa)) noinline notrace void
komb_rwsem_up_write(struct komb_rwsem *lock)
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
		if (lock->wlocked == _KOMB_RWSEM_W_LOCKED)
		{
			//print_debug("Write unlock\n");
			WRITE_ONCE(lock->wlocked, 0);
		}
		else if (lock->wlocked == _KOMB_RWSEM_W_COMBINER) {
#ifdef KOMB_STATS
			this_cpu_inc(ooo_unlocks);
#endif

			lock->wlocked = _KOMB_RWSEM_W_OOO;
			print_debug("OOO unlock\n");
		} else
			BUG_ON(true);
		return;
	}

	LOCK_END_TIMING_PER_CPU_DISABLE(combiner_loop);

	LOCK_START_TIMING_PER_CPU_DISABLE(combiner_loop);
#ifdef DEBUG_KOMB
	if (lock->wlocked != _KOMB_RWSEM_W_COMBINER)
		BUG_ON(true);

	BUG_ON(current->komb_curr_waiter_task == NULL);

	BUG_ON(current->komb_next_waiter_task == NULL);

	if (max_idx < 0) {
		BUG_ON(true);
	}

#endif
	if (my_idx < max_idx) {
#ifdef KOMB_STATS
		this_cpu_inc(ooo_unlocks);
#endif
		lock->wlocked = _KOMB_RWSEM_W_OOO;
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

	if (next_node == NULL || next_node->next_rwsem == NULL ||
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

void komb_rwsem_init(void)
{
	#ifdef BRAVO
	global_vr_table = vzalloc(TABLE_SIZE * sizeof(struct komb_rwsem*));
#endif
}

void __init_komb_rwsem(struct komb_rwsem *lock, const char *name,
		       struct lock_class_key *key)
{
	atomic_long_set(&lock->cnts, 0);
	aqs_spin_lock_init(&lock->reader_wait_lock);
	lock->writer_tail = NULL;
}

int __must_check komb_rwsem_down_read_killable(struct komb_rwsem *lock)
{
	komb_rwsem_down_read(lock);
	return 0;
}

int komb_rwsem_down_read_trylock(struct komb_rwsem *lock)
{
#ifdef BRAVO
	if(READ_ONCE(lock->rbias)) {
		uint64_t **slot = NULL;
		uint64_t new_val = (uint64_t)&current->pid;
		u32 id = hash(new_val);
		slot = &global_vr_table[V(id)];

		if(cmpxchg(slot, NULL, new_val) == NULL) {
			if(READ_ONCE(lock->rbias))
					return 1;
			(void)xchg(slot, NULL);
		}
	}
#endif

	u64 cnts =
		atomic_long_add_return_acquire(_KOMB_RWSEM_R_BIAS, &lock->cnts);
	if (likely(!(cnts & _KOMB_RWSEM_W_WMASK)))
	{
#ifdef BRAVO
		if (((this_cpu_inc_return(check_bias) % CHECK_FOR_BIAS) == 0) &&
		    (!READ_ONCE(lock->rbias) && rdtsc() >= lock->inhibit_until))
			WRITE_ONCE(lock->rbias, 1);
#endif

		return 1;
	}
	(void)atomic_long_sub_return_release(_KOMB_RWSEM_R_BIAS, &lock->cnts);

	return 0;
}

int __must_check komb_rwsem_down_write_killable(struct komb_rwsem *lock)
{
	komb_rwsem_down_write(lock);
	return 0;
}

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
int komb_rwsem_down_write_trylock(struct komb_rwsem *lock)
{
	int val = (atomic_long_cmpxchg_acquire(&lock->cnts, 0,
					    _KOMB_RWSEM_W_LOCKED) == 0);

#ifdef BRAVO
	if(val)
		wait_for_visible_readers(lock);
#endif

	return val;
}

void komb_rwsem_down_read_nested(struct komb_rwsem *sem, int level)
{
	komb_rwsem_down_read(sem);
}
void komb_rwsem_down_write_nested(struct komb_rwsem *sem, int level)
{
	komb_rwsem_down_write(sem);
}

int komb_rwsem_down_write_killable_nested(struct komb_rwsem *sem, int level)
{
	komb_rwsem_down_write_killable(sem);
	return 0;
}

/*
 * TODO:
 * Unimplemented functions
 */

/*
 * downgrade write lock to read lock
 */
void komb_rwsem_downgrade_write(struct komb_rwsem *lock)
{
	BUG_ON(true);
}

#
