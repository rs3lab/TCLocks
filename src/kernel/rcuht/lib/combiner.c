// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022 Vishal Gupta, Kumar Kartikeya Dwivedi

#if KERNEL_SYNCSTRESS
#include "lib/combiner.h"
#else
#include <linux/combiner.h>
long komb_batch_size = 1024; //262144;
#endif

#if KOMB_STATS
DEFINE_PER_CPU_ALIGNED(uint64_t, combiner_count);
DEFINE_PER_CPU_ALIGNED(uint64_t, waiter_combined);
DEFINE_PER_CPU_ALIGNED(uint64_t, ooo_combiner_count);
DEFINE_PER_CPU_ALIGNED(uint64_t, ooo_waiter_combined);
DEFINE_PER_CPU_ALIGNED(uint64_t, ooo_unlocks);
#endif

#include <linux/syscalls.h>
/*
 * incoming_rsp_ptr -> rdi
 * outgoing_rsp_ptr -> rsi
 *
 * Assume IRQ is disabled. Will enable when returning.
 */
#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace void
komb_context_switch(void *incoming_rsp_ptr, void *outgoing_rsp_ptr)
{
	asm volatile("pushq %%rbp\n"
		     "pushq %%rbx\n"
		     "pushq %%r12\n"
		     "pushq %%r13\n"
		     "pushq %%r14\n"
		     "pushq %%r15\n"
		     "movq %%rsp, (%%rsi)\n"
		     "movq (%%rdi), %%rsp\n"
		     "popq %%r15\n"
		     "popq %%r14\n"
		     "popq %%r13\n"
		     "popq %%r12\n"
		     "popq %%rbx\n"
		     "popq %%rbp\n"
		     :
		     :
		     : "memory");
}
#pragma GCC pop_options

#if KERNEL_SYNCSTRESS == 0
#if KOMB_STATS
SYSCALL_DEFINE0(komb_stats)
{
	int i;
	uint64_t total_counters[16] = { 0 };

	printk(KERN_ALERT "======== KOMB spinlock stats ========\n");
	for_each_online_cpu (i) {
		total_counters[0] += per_cpu(combiner_count, i);
		total_counters[1] += per_cpu(waiter_combined, i);
		total_counters[2] += per_cpu(ooo_unlocks, i);
		total_counters[3] += per_cpu(ooo_combiner_count, i);
		total_counters[4] += per_cpu(ooo_waiter_combined, i);
//		total_counters[5] += per_cpu(lock_not_in_task, i);
//		total_counters[6] += per_cpu(mutex_combiner_count, i);
//		total_counters[7] += per_cpu(mutex_waiter_combined, i);
//		total_counters[8] += per_cpu(mutex_ooo_unlocks, i);
//		total_counters[9] += per_cpu(mutex_ooo_combiner_count, i);
//		total_counters[10] += per_cpu(mutex_ooo_waiter_combined, i);
//		total_counters[11] += per_cpu(rwsem_combiner_count, i);
//		total_counters[12] += per_cpu(rwsem_waiter_combined, i);
//		total_counters[13] += per_cpu(rwsem_ooo_unlocks, i);
//		total_counters[14] += per_cpu(rwsem_ooo_combiner_count, i);
//		total_counters[15] += per_cpu(rwsem_ooo_waiter_combined, i);
	}

	printk(KERN_ALERT "Combiner_count: %lld\n", total_counters[0]);
	printk(KERN_ALERT "waiter_combined: %lld\n", total_counters[1]);
	printk(KERN_ALERT "ooo_unlocks: %lld\n", total_counters[2]);
	printk(KERN_ALERT "ooo_combiner_count: %lld\n", total_counters[3]);
	printk(KERN_ALERT "ooo_waiter_combined: %lld\n", total_counters[4]);
	printk(KERN_ALERT "lock_not_in_task: %lld\n", total_counters[5]);
//	printk(KERN_ALERT "mutex_Combiner_count: %lld\n", total_counters[6]);
//	printk(KERN_ALERT "mutex_waiter_combined: %lld\n", total_counters[7]);
//	printk(KERN_ALERT "mutex_ooo_unlocks: %lld\n", total_counters[8]);
//	printk(KERN_ALERT "mutex_ooo_combiner_count: %lld\n", total_counters[9]);
//	printk(KERN_ALERT "mutex_ooo_waiter_combined: %lld\n", total_counters[10]);
//	printk(KERN_ALERT "rwsem_Combiner_count: %lld\n", total_counters[11]);
//	printk(KERN_ALERT "rwsem_waiter_combined: %lld\n", total_counters[12]);
//	printk(KERN_ALERT "rwsem_ooo_unlocks: %lld\n", total_counters[13]);
//	printk(KERN_ALERT "rwsem_ooo_combiner_count: %lld\n", total_counters[14]);
//	printk(KERN_ALERT "rwsem_ooo_waiter_combined: %lld\n", total_counters[15]);

	return 0;
}

SYSCALL_DEFINE0(komb_clear_stats)
{
	int i;

	printk(KERN_ALERT "======== KOMB stats cleared ========\n");
	for_each_online_cpu (i) {
		*per_cpu_ptr(&combiner_count, i) = 0;
		*per_cpu_ptr(&waiter_combined, i) = 0;
		*per_cpu_ptr(&ooo_unlocks, i) = 0;
		*per_cpu_ptr(&ooo_combiner_count, i) = 0;
		*per_cpu_ptr(&ooo_waiter_combined, i) = 0;
//		*per_cpu_ptr(&lock_not_in_task, i) = 0;
//		*per_cpu_ptr(&mutex_combiner_count, i) = 0;
//		*per_cpu_ptr(&mutex_waiter_combined, i) = 0;
//		*per_cpu_ptr(&mutex_ooo_unlocks, i) = 0;
//		*per_cpu_ptr(&mutex_ooo_combiner_count, i) = 0;
//		*per_cpu_ptr(&mutex_ooo_waiter_combined, i) = 0;
//		*per_cpu_ptr(&rwsem_combiner_count, i) = 0;
//		*per_cpu_ptr(&rwsem_waiter_combined, i) = 0;
//		*per_cpu_ptr(&rwsem_ooo_unlocks, i) = 0;
//		*per_cpu_ptr(&rwsem_ooo_combiner_count, i) = 0;
//		*per_cpu_ptr(&rwsem_ooo_waiter_combined, i) = 0;

	}
	return 0;
}
#else
SYSCALL_DEFINE0(komb_stats)
{
	return 0;
}

SYSCALL_DEFINE0(komb_clear_stats)
{
	return 0;
}
#endif
#endif
