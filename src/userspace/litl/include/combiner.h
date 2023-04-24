// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022 Vishal Gupta, Kumar Kartikeya Dwivedi

/*
 * incoming_rsp_ptr -> rdi
 * outgoing_rsp_ptr -> rsi
 *
 * Assume IRQ is disabled. Will enable when returning.
 */

#define NUMA_AWARE 1

#define komb_switch_to_shadow_stack()                                          \
    ({                                                                         \
        asm volatile("pushq %%rbp\n"                                           \
                     "pushq %%rbx\n"                                           \
                     "pushq %%r12\n"                                           \
                     "pushq %%r13\n"                                           \
                     "pushq %%r14\n"                                           \
                     "pushq %%r15\n"                                           \
                     :                                                         \
                     :                                                         \
                     : "memory");                                              \
        asm volatile("callq %P0\n"                                             \
                     "movq %%rsp, %c1(%%rax)\n"                                \
                     :                                                         \
                     : "i"(get_komb_mutex_node),                               \
                       "i"(offsetof(struct komb_mutex_node, rsp))              \
                     : "memory");                                              \
        asm volatile("callq %P0\n"                                             \
                     "movq (%%rax), %%rsp\n"                                   \
                     "pushq %%rdi\n"                                           \
                     :                                                         \
                     : "i"(get_shadow_stack_ptr)                               \
                     : "memory");                                              \
    })

#define komb_switch_from_shadow_stack()                                        \
    ({                                                                         \
        asm volatile("popq %%rdi\n"                                            \
                     "callq %P0\n"                                             \
                     "movq %%rsp, (%%rax)\n"                                   \
                     :                                                         \
                     : "i"(get_shadow_stack_ptr)                               \
                     : "memory");                                              \
        asm volatile("callq %P0\n"                                             \
                     "movq %c1(%%rax), %%rsp\n"                                \
                     :                                                         \
                     : "i"(get_komb_mutex_node),                               \
                       "i"(offsetof(struct komb_mutex_node, rsp))              \
                     : "memory");                                              \
        asm volatile("popq %%r15\n"                                            \
                     "popq %%r14\n"                                            \
                     "popq %%r13\n"                                            \
                     "popq %%r12\n"                                            \
                     "popq %%rbx\n"                                            \
                     "popq %%rbp\n"                                            \
                     :                                                         \
                     :                                                         \
                     : "memory");                                              \
    })

void komb_context_switch(void *incoming_rsp_ptr, void *outgoing_rsp_ptr);
