// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022 Vishal Gupta, Kumar Kartikeya Dwivedi

/*
 * incoming_rsp_ptr -> rdi
 * outgoing_rsp_ptr -> rsi
 *
 * Assume IRQ is disabled. Will enable when returning.
 */
#include <linux/kernel.h>
#include <linux/percpu.h>

#define DSM_DEBUG 0
#define DEBUG_KOMB 0 
#define KOMB_STATS 1

#define NUMA_AWARE 1
#define PREFETCHING 1
#define WWJUMP 1 

#define NUM_PREFETCH_LINES 6

#define SIZE_OF_SHADOW_STACK 4096
#define IRQ_NUMA_NODE 255
#define UINT64_MAX 0xffffffffffffffffL

#define LOCK_START_TIMING_DISABLE(name, start)
#define LOCK_START_TIMING_PER_CPU_DISABLE(name)
#define LOCK_END_TIMING_DISABLE(name, start)
#define LOCK_END_TIMING_PER_CPU_DISABLE(name)

#define komb_switch_to_shadow_stack()                                          \
	({                                                                     \
		asm volatile("pushq %%rbp\n"                                   \
			     "pushq %%rbx\n"                                   \
			     "pushq %%r12\n"                                   \
			     "pushq %%r13\n"                                   \
			     "pushq %%r14\n"                                   \
			     "pushq %%r15\n"                                   \
			     :                                                 \
			     :                                                 \
			     : "memory");                                      \
		asm volatile("callq %P0\n"                                     \
			     "movq %%rsp, %c1(%%rax)\n"                        \
			     :                                                 \
			     : "i"(get_komb_mutex_node),                       \
			       "i"(offsetof(struct komb_mutex_node, rsp))      \
			     : "memory");                                      \
		asm volatile("callq %P0\n"                                     \
			     "movq (%%rax), %%rsp\n"                           \
			     "pushq %%rdi\n"                                   \
			     :                                                 \
			     : "i"(get_shadow_stack_ptr)                       \
			     : "memory");                                      \
	})

#define komb_switch_from_shadow_stack()                                        \
	({                                                                     \
		asm volatile("popq %%rdi\n"                                    \
			     "callq %P0\n"                                     \
			     "movq %%rsp, (%%rax)\n"                           \
			     :                                                 \
			     : "i"(get_shadow_stack_ptr)                       \
			     : "memory");                                      \
		asm volatile("callq %P0\n"                                     \
			     "movq %c1(%%rax), %%rsp\n"                        \
			     :                                                 \
			     : "i"(get_komb_mutex_node),                       \
			       "i"(offsetof(struct komb_mutex_node, rsp))      \
			     : "memory");                                      \
		asm volatile("popq %%r15\n"                                    \
			     "popq %%r14\n"                                    \
			     "popq %%r13\n"                                    \
			     "popq %%r12\n"                                    \
			     "popq %%rbx\n"                                    \
			     "popq %%rbp\n"                                    \
			     :                                                 \
			     :                                                 \
			     : "memory");                                      \
	})

extern long komb_batch_size;

void komb_context_switch(void *incoming_rsp_ptr, void *outgoing_rsp_ptr);

#if KOMB_STATS
DECLARE_PER_CPU_ALIGNED(u64, combiner_count);
DECLARE_PER_CPU_ALIGNED(u64, waiter_combined);
DECLARE_PER_CPU_ALIGNED(u64, ooo_combiner_count);
DECLARE_PER_CPU_ALIGNED(u64, ooo_waiter_combined);
DECLARE_PER_CPU_ALIGNED(u64, ooo_unlocks);
//DECLARE_PER_CPU_ALIGNED(u64, lock_not_in_task);
//DECLARE_PER_CPU_ALIGNED(u64, mutex_combiner_count);
//DECLARE_PER_CPU_ALIGNED(u64, mutex_waiter_combined);
//DECLARE_PER_CPU_ALIGNED(u64, mutex_ooo_combiner_count);
//DECLARE_PER_CPU_ALIGNED(u64, mutex_ooo_waiter_combined);
//DECLARE_PER_CPU_ALIGNED(u64, mutex_ooo_unlocks);
//DECLARE_PER_CPU_ALIGNED(u64, rwsem_combiner_count);
//DECLARE_PER_CPU_ALIGNED(u64, rwsem_waiter_combined);
//DECLARE_PER_CPU_ALIGNED(u64, rwsem_ooo_combiner_count);
//DECLARE_PER_CPU_ALIGNED(u64, rwsem_ooo_waiter_combined);
//DECLARE_PER_CPU_ALIGNED(u64, rwsem_ooo_unlocks);
#endif
