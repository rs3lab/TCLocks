// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2024 Vishal Gupta, Kumar Kartikeya Dwivedi, Sixiao Xu

#include "spinlock/swilock.h"
#include "spinlock/komb.h"
#include "timing_stats.h"

#include <linux/topology.h>
#include <linux/vmalloc.h>
#include <linux/percpu-defs.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/atomic.h>

#define DELEGATION_CPU 19

#if LOCK_MEASURE_TIME
static DEFINE_PER_CPU_ALIGNED(uint64_t, delegation_loop);
#endif

#if DSM_DEBUG
#define print_debug(fmt, ...)                                                  \
	({                                                                     \
		printk(KERN_EMERG "[%d] komb (%s): " fmt, smp_processor_id(),  \
		       __func__, ##__VA_ARGS__);                               \
	})
#else
#define print_debug(fmt, ...)
#endif

#if KERNEL_SYNCSTRESS
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
		(typeof(*ptr)) VAL;                                            \
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
		(typeof(*ptr)) VAL;                                            \
	})
#endif

static struct task_struct *hthread;

DEFINE_PER_CPU_SHARED_ALIGNED(struct swi_node, swi_nodes[MAX_NODES]);

DEFINE_PER_CPU_SHARED_ALIGNED(struct delegation_request, delegation_requests);
DEFINE_PER_CPU_SHARED_ALIGNED(struct delegation_server, delegation_servers);

__attribute__((noipa)) noinline notrace static void *get_shadow_stack_ptr(void)
{
	struct delegation_request *ptr = this_cpu_ptr(&delegation_requests);
	return &(ptr->shadow_stack_ptr);
}

__attribute__((noipa)) noinline notrace static struct delegation_request *
get_delegation_request(void)
{
	return this_cpu_ptr(&delegation_requests);
}

/*static noinline bool get_ith_bit(u64 number, int pos)
{
	return (number << (63L - pos)) >> 63L;
}

static noinline u64 flip_ith_bit(u64 number, int pos)
{
	return number ^ (1L << pos);
}*/

void acquire_tas(struct qspinlock *lock)
{
	struct swi_node *swi_node;

	while (1) {
		if (atomic_cmpxchg_acquire(&lock->val, 0, _Q_LOCKED_VAL) == 0)
			break;
	}
	// print_debug("delegate thread lock acquired");
	// Mark delegate, to ensure unlock with delegate_finish
	swi_node = this_cpu_ptr(&swi_nodes[0]);
	swi_node->is_delegate = true;
}

void release_tas(struct qspinlock *lock)
{
	struct swi_node *swi_node = this_cpu_ptr(&swi_nodes[0]);
	swi_node->is_delegate = false;
	// Release TAS lock
	smp_store_release(&lock->locked, 0);
	// print_debug("delegate thread lock released");
}

/**************************************************************
 * 
 * Server thread functions
 * 
 * **************************************************************/

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace static void
swi_delegate_execute(struct delegation_request *request)
{
	void *incoming_rsp_ptr, *outgoing_rsp_ptr;
	struct delegation_server *server_ptr = per_cpu_ptr(&delegation_servers, DELEGATION_CPU);

#if DEBUG_DELEGATION
	print_debug("delegate [%d]", request->cpu_id);
#endif

	server_ptr->cur_client_cpu_id = request->cpu_id;

	// Server -> client thread
	incoming_rsp_ptr = &(request->main_stack_ptr);
	outgoing_rsp_ptr = &(server_ptr->server_stack_ptr);

	BUG_ON(*(char *)incoming_rsp_ptr == 0);
	//BUG_ON(*(char *)outgoing_rsp_ptr == 0);

	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
}
#pragma GCC pop_options

/*****
 * Execute requests on a socket, update response together
 * ******/
void swi_delegate_execute_socket(int socket_id)
{
#if ENABLE_JUMP == 0
	BUG_ON(true); //TODO: Fix the responses
	//u64 updated_response;
	int socket_offset, i;

	//updated_response = server_ptr->responses[socket_id].toggle;
	// Iterate through each core on the socket
	socket_offset = socket_id * CORES_PER_SOCKET;
	for (i = 0; i < CORES_PER_SOCKET; i++) {
		struct delegation_request *request =
			per_cpu_ptr(&delegation_requests, i + socket_offset);
		// Check if has request
		// i.e. toggle bit differs in request and reponse
		if (READ_ONCE(request->toggle) !=
		    READ_ONCE(server_ptr->responses[i].toggle)) {
			// LOCK_START_TIMING_PER_CPU(delegation_loop);
			swi_delegate_execute(request);
			// Update toggle bit
			// updated_response = flip_ith_bit(updated_response, i);
			// LOCK_END_TIMING_PER_CPU(delegation_loop);
		}
	}
	// Write response
	//WRITE_ONCE(server_ptr->responses[socket_id].toggle, updated_response);

#else
	int socket_offset, i;
	bool found_one;
	struct delegation_server *server_ptr = per_cpu_ptr(&delegation_servers, DELEGATION_CPU);

	//server_ptr->cur_updated_response = server_ptr->responses[socket_id].toggle;
	server_ptr->cur_socket_id = socket_id;
	// Find first core with request on current socket
	socket_offset = socket_id * CORES_PER_SOCKET;
	server_ptr->prev_client_cpu_id = -1;
	found_one = false;
	for (i = 0; i < CORES_PER_SOCKET; i++) {
		struct delegation_request *request =
			per_cpu_ptr(&delegation_requests, i + socket_offset);
		// Check if has request
		// i.e. toggle bit differs in request and reponse
		if (READ_ONCE(request->toggle) != READ_ONCE(server_ptr->responses[i].toggle)) {
			server_ptr->cur_client_cpu_id = request->cpu_id;
			server_ptr->cur_client_cpu_id_on_socket =
				request->cpu_id_on_socket;
			found_one = true;
			swi_delegate_execute(request);
			// jump to next core will be handled in delegate_finish()
			break;
		}
	}
	if(found_one) {
		i = server_ptr->prev_client_cpu_id;
		BUG_ON(i == -1);
		WRITE_ONCE(server_ptr->responses[i].toggle, !(READ_ONCE(server_ptr->responses[i].toggle))); 	
	}

	// Write response
	//WRITE_ONCE(server_ptr->responses[socket_id].toggle,
	//	   server_ptr->cur_updated_response);
#endif
}

int swi_delegate_thread(void *args)
{
	int i;

	print_debug("swilock delegate thread start");
	while (!kthread_should_stop()) {
#if DEBUG_DELEGATION
		print_debug("delegation thread wake up");
#endif
		if (kthread_should_stop()) {
			break;
		}
		// acquire_tas(server_ptr->lock);

		while (!kthread_should_stop()) {
			// Iterate through each socket
			for (i = 0; i < MAX_CORES / CORES_PER_SOCKET; i++) {
				swi_delegate_execute_socket(i);
				if (need_resched()) {
					preempt_enable();
					schedule();
					preempt_disable();
				}
			}
		}
#if DEBUG_DELEGATION
		print_debug("delegation thread release lock");
#endif
		// release_tas(server_ptr->lock);
	}
	return 0;
}

/**************************************************************
 * 
 * Client thread functions
 * 
 * **************************************************************/

/******
 * 3. Set up request, wait for response
 * *******/
__attribute__((noipa)) noinline notrace static void
__swilock_delegate_slowpath(struct qspinlock *lock)
{
	struct delegation_request *request;
        struct delegation_server *server_ptr;
	int socket_id;
	int cpu_id_on_socket;
	bool toggle;

	server_ptr = per_cpu_ptr(&delegation_servers, DELEGATION_CPU);

	request = this_cpu_ptr(&delegation_requests);
	socket_id = request->socket_id;
	cpu_id_on_socket = request->cpu_id_on_socket;

	// Read toggle bit in response
	toggle = !request->toggle;

	// LOCK_START_TIMING_PER_CPU(delegation_loop);
	// Set request toggle bit
	WRITE_ONCE(request->toggle, toggle);

	// Wait for the server to execute request
	// i.e. toggle bit != toggle bit on server
#if DEBUG_DELEGATION
	print_debug("wait on toggle bit [%d]", toggle);
#endif
	while (READ_ONCE(request->toggle) != READ_ONCE(server_ptr->responses[cpu_id_on_socket].toggle)) {
		BUG_ON(READ_ONCE(request->toggle) != toggle);
		cpu_relax();
		if (need_resched()) {
			preempt_enable();
			schedule();
			preempt_disable();
		}
	}
	// LOCK_END_TIMING_PER_CPU(delegation_loop);
#if DEBUG_DELEGATION
	print_debug("wake up");
#endif
}

/*******
 * 2. Client main -> shadow thread
 * ******/
#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace void
swilock_delegate_slowpath(struct qspinlock *lock)
{
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
		     : "i"(get_delegation_request),
		       "i"(offsetof(struct delegation_request, main_stack_ptr))
		     : "memory");
	asm volatile("callq %P0\n"
		     "movq (%%rax), %%rsp\n"
		     :
		     : "i"(get_shadow_stack_ptr)
		     : "memory");

	__swilock_delegate_slowpath(lock);

	asm volatile("callq %P0\n"
		     "movq %%rsp, (%%rax)\n"
		     :
		     : "i"(get_shadow_stack_ptr)
		     : "memory");
	asm volatile("callq %P0\n"
		     "movq %c1(%%rax), %%rsp\n"
		     :
		     : "i"(get_delegation_request),
		       "i"(offsetof(struct delegation_request, main_stack_ptr))
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
#pragma GCC pop_options

///*********
// * 1. Called by client thread, to replace lock()
// * ********/
//__attribute__((noipa)) noinline notrace void
//swilock_delegate(struct qspinlock *lock)
//{
//	swilock_delegate_slowpath(lock);
//}

/*********
 * Called by client thread, to replace unlock()
 * Executed by server thread
 * ********/
__attribute__((noipa)) noinline notrace void
swilock_delegate_finish(struct qspinlock *lock)
{
	struct delegation_request *request;
	struct delegation_server *server_ptr;
	void *incoming_rsp_ptr, *outgoing_rsp_ptr;

#if ENABLE_JUMP
	bool found_next;
	struct delegation_request *next_request;
	int socket_id, socket_offset, i;
	void *rsp_ptr;
#endif
	server_ptr = per_cpu_ptr(&delegation_servers, DELEGATION_CPU);

#if DEBUG_DELEGATION
	print_debug("finish [%d]", server_ptr->cur_client_cpu_id);
#endif
	request = per_cpu_ptr(&delegation_requests,
			      server_ptr->cur_client_cpu_id);

#if ENABLE_JUMP == 0
	// Client -> server thread
	incoming_rsp_ptr = &(server_ptr->server_stack_ptr);
	outgoing_rsp_ptr = &(request->main_stack_ptr);

	BUG_ON(*(char *)incoming_rsp_ptr == 0);

	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
	return;
#else
	// Update temporary toggle bit
	//server_ptr->cur_updated_response =
	//	flip_ith_bit(server_ptr->cur_updated_response,
	//		     server_ptr->cur_client_cpu_id_on_socket);
	// Find next core with request on same socket
	found_next = false;
	next_request = NULL;
	socket_id = server_ptr->cur_socket_id;
	socket_offset = socket_id * CORES_PER_SOCKET;
	i = server_ptr->cur_client_cpu_id_on_socket + 1;

	for (; i < CORES_PER_SOCKET; i++) {
		next_request =
			per_cpu_ptr(&delegation_requests, i + socket_offset);
		// Check if has request
		// i.e. toggle bit differs in request and reponse
		if (READ_ONCE(next_request->toggle) !=
		    READ_ONCE(server_ptr->responses[i].toggle)) {
			found_next = true;
			break;
		}
	}

	server_ptr->prev_client_cpu_id = server_ptr->cur_client_cpu_id;

	if (found_next) {
#if DEBUG_DELEGATION
		print_debug("jump [%d]->[%d]", request->cpu_id,
			    next_request->cpu_id);
#endif
#if ENABLE_PREFETCH
		rsp_ptr = next_request->main_stack_ptr;
		prefetchw(rsp_ptr);
		for (i = 1; i < NUM_PREFETCH_LINES; i++)
			prefetchw(rsp_ptr + (64 * i));
#endif
		server_ptr->cur_client_cpu_id = next_request->cpu_id;
		server_ptr->cur_client_cpu_id_on_socket =
			next_request->cpu_id_on_socket;
		// Client -> next client with request on same socket
		incoming_rsp_ptr = &(next_request->main_stack_ptr);
	} else {
#if DEBUG_DELEGATION
		print_debug("back [%d]->[s]",
			    server_ptr->cur_client_cpu_id_on_socket);
#endif
		// Jump back to server
		incoming_rsp_ptr = &(server_ptr->server_stack_ptr);
	}

	// LOCK_END_TIMING_PER_CPU(delegation_loop);

	// LOCK_START_TIMING_PER_CPU(delegation_loop);

	outgoing_rsp_ptr = &(request->main_stack_ptr);

	BUG_ON(*(char *)incoming_rsp_ptr == 0);

	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
	return;
#endif
}

/**************************************************************
 * 
 * Init & exit
 * 
 * ************************************************************/

void swilock_delegate_init(struct qspinlock *lock)
{
	int i;
	void *stack_ptr;
	struct delegation_request *ptr;
	struct delegation_server *server_ptr;
	print_debug("swilock delegate thread init\n");

	// Init delegation request for each cpu
	for_each_possible_cpu (i) {
		stack_ptr = vzalloc(SIZE_OF_SHADOW_STACK);
		BUG_ON(stack_ptr == NULL);
		ptr = per_cpu_ptr(&delegation_requests, i);
		ptr->base_ptr = stack_ptr + SIZE_OF_SHADOW_STACK;
		ptr->shadow_stack_ptr = NULL;
		ptr->shadow_stack_ptr = stack_ptr + SIZE_OF_SHADOW_STACK - 8;
		ptr->cpu_id = i;
		ptr->socket_id = i / CORES_PER_SOCKET;
		ptr->cpu_id_on_socket =
			i; // - ptr->socket_id * CORES_PER_SOCKET;
		ptr->toggle = false;

#if LOCK_MEASURE_TIME
		*per_cpu_ptr(&delegation_loop, i) = UINT64_MAX;
#endif
	}

	// Init server info
	/*server_ptr = (struct delegation_server *)vzalloc(
		sizeof(struct delegation_server));*/
	server_ptr = per_cpu_ptr(&delegation_servers, DELEGATION_CPU);
	server_ptr->lock = lock;
	server_ptr->server_stack_ptr = NULL;
	server_ptr->prev_client_cpu_id = -1;
	for (i = 0; i < MAX_CORES; i++) {
		//server_ptr->responses[i] = (struct response *)vzalloc(sizeof(struct response));
		server_ptr->responses[i].toggle = false;
	}

	// Init server thread
	hthread = kthread_create(swi_delegate_thread, NULL,
				 "swi_delegate_thread");
	// Bind delegation thread to core 19 (current VM: 20 cores)
	kthread_bind(hthread, DELEGATION_CPU);
	if (hthread) {
		wake_up_process(hthread);
	} else {
		printk(KERN_ERR "failed to create swilock delegate thread\n");
	}
}

void swilock_delegate_exit(void)
{
	//msleep(2000);  // Wait for remaining task to finish
	int ret = kthread_stop(hthread);
	int i;

	print_debug("swilock delegate thread exit\n");

	if (ret) {
		printk(KERN_ALERT "swilock delegate thread returned error %d\n",
		       ret);
	}

	for_each_possible_cpu (i) {
		vfree(per_cpu_ptr(&delegation_requests, i)->base_ptr -
		      SIZE_OF_SHADOW_STACK);
	}
}

__attribute__((noipa)) noinline notrace void swi_lock(struct qspinlock *lock)
{
	int i;
	struct delegation_server *server_ptr = per_cpu_ptr(&delegation_servers, DELEGATION_CPU);

	swilock_delegate_slowpath(lock);
	BUG_ON(smp_processor_id() != DELEGATION_CPU); // Executed on delegation CPU
	i = server_ptr->prev_client_cpu_id;
	BUG_ON(i == DELEGATION_CPU);
	if(i != -1)
		WRITE_ONCE(server_ptr->responses[i].toggle, !(READ_ONCE(server_ptr->responses[i].toggle))); 
}

__attribute__((noipa)) noinline notrace void swi_unlock(struct qspinlock *lock)
{
	BUG_ON(smp_processor_id() != DELEGATION_CPU); // Executed on delegation CPU
	swilock_delegate_finish(lock);
}

void swilock_helper_exit(void)
{
	int ret = kthread_stop(hthread);
	if (ret)
		printk(KERN_ALERT "swilock helper thread returned error %d\n",
		       ret);
}
