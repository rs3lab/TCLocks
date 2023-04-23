// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022 Vishal Gupta, Kumar Kartikeya Dwivedi

#ifdef KERNEL_SYNCSTRESS
#include "lib/combiner.h"
#else
#include <linux/combiner.h>
long komb_batch_size = 1024; //262144;
#endif

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
