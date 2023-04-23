// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022 Vishal Gupta, Kumar Kartikeya Dwivedi

/*
 * TODO: (Performance optimiztion)
 * Fix the index part when same is acquired multiple times.
 * Currently nested locking becomes TTAS lock.
 */

#include "rwlock/komb_rwlock.h"
#include <linux/topology.h>
#include <linux/vmalloc.h>

#define SIZE_OF_SHADOW_STACK 8192L

#ifdef NUMA_AWARE
/*
 * Create a queue which is used by the combiner to store nodes which belong to 
 * different socket.
 */
static DEFINE_PER_CPU_ALIGNED(struct komb_rwlock_node *, local_queue_head);
static DEFINE_PER_CPU_ALIGNED(struct komb_rwlock_node *, local_queue_tail);
#endif

struct shadow_stack {
	/*
	 * ptr points to the base of the shadow stack.
	 */
	void *ptr;

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
};

/*
 * Used by the combiner to identify for which CPU is the critical section 
 * currently executing.
 */
static DEFINE_PER_CPU_ALIGNED(uint8_t, current_critical_section_cpu);

/*
 * Used by all threads to add itself to the queue on the slowpath.
 */
static DEFINE_PER_CPU_ALIGNED(struct komb_rwlock_node,
			      komb_rwlock_nodes[MAX_NODES]);

/*
 * 8KB shadow stack used by all the threads. For waiter threads, switch to 
 * shadow stack, because of IRQs. For combiner thread, switch to shadow stack 
 * to handle nesting
 */
static DEFINE_PER_CPU_ALIGNED(struct shadow_stack, local_shadow_stack);
static DEFINE_PER_CPU_ALIGNED(uint64_t, local_shadow_stack_ptr);

extern long komb_batch_size;

#define _Q_LOCKED_PENDING_MASK (_Q_LOCKED_MASK | _Q_PENDING_MASK)
#define _Q_LOCKED_COMBINER_VAL 3
#define _Q_UNLOCKED_OOO_VAL 7 //Unlocked a lock out-of-order

/*
 * We must be able to distinguish between no-tail and the tail at 0:0,
 * therefore increment the cpu number by one.
 */
static inline __pure u32 encode_tail(int cpu, int idx)
{
	u32 tail;

	idx = 0;
	BUG_ON(idx > 3);
	tail = (cpu + 1) << _Q_TAIL_CPU_OFFSET;
	tail |= idx << _Q_TAIL_IDX_OFFSET; /* assume < 4 */

	return tail;
}

static inline __pure struct komb_rwlock_node *decode_tail(u32 tail)
{
	int cpu = (tail >> _Q_TAIL_CPU_OFFSET) - 1;
	int idx = (tail & _Q_TAIL_IDX_MASK) >> _Q_TAIL_IDX_OFFSET;

	return per_cpu_ptr(&komb_rwlock_nodes[0], cpu);
}

static inline __pure u32 get_cpu_from_tail(u32 tail)
{
	return ((tail >> _Q_TAIL_CPU_OFFSET) - 1);
}

static __always_inline void
clear_locked_set_completed(struct komb_rwlock_node *lock)
{
	WRITE_ONCE(lock->locked_completed, 1);
}

static __always_inline void
clear_pending_set_locked(struct orig_qspinlock *lock)
{
	WRITE_ONCE(lock->locked_pending, _Q_LOCKED_VAL);
}

static __always_inline u32 xchg_tail(struct orig_qspinlock *lock, u32 tail)
{
	return ((u32)xchg(&lock->tail, tail >> _Q_TAIL_OFFSET))
	       << _Q_TAIL_OFFSET;
}

static __always_inline u32 cmpxchg_tail(struct orig_qspinlock *lock, u32 tail,
					u32 new_tail)
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
static __always_inline void clear_pending(struct orig_qspinlock *lock)
{
	atomic_andnot(_Q_PENDING_VAL, &lock->val);
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
 * queued_fetch_set_pending_acquire - fetch the whole lock value and set pending
 * @lock : Pointer to queued spinlock structure
 * Return: The previous lock value
 *
 * *,*,* -> *,1,*
 */
static __always_inline u32
komb_fetch_set_pending_acquire(struct orig_qspinlock *lock)
{
	return atomic_fetch_or_acquire(_Q_PENDING_VAL, &lock->val);
}

#ifdef NUMA_AWARE
__always_inline static void add_to_local_queue(struct komb_rwlock_node *node)
{
	struct komb_rwlock_node **head, **tail;

	head = this_cpu_ptr(&local_queue_head);
	tail = this_cpu_ptr(&local_queue_tail);

	if (*head == NULL) {
		*head = node;
		*tail = node;
	} else {
		(*tail)->next = node;
		*tail = node;
	}
}
#endif

__always_inline static struct komb_rwlock_node *
get_next_node(struct komb_rwlock_node *my_node)
{
#ifdef NUMA_AWARE
	struct komb_rwlock_node **local_head, **local_tail;
	struct komb_rwlock_node *curr_node, *next_node;

	curr_node = my_node;
	next_node = curr_node->next;

	while (true) {
		if (next_node == NULL || next_node->next == NULL)
			goto next_node_null;

		if (next_node->socket_id == numa_node_id()) {
			prefetchw(per_cpu_ptr(&komb_rwlock_nodes[0],
					      next_node->cpuid)
					  ->rsp);
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

__always_inline static void execute_cs(struct orig_qspinlock *lock,
				       struct komb_rwlock_node *curr_node)
{
	void *incoming_rsp_ptr, *outgoing_rsp_ptr;

	*this_cpu_ptr(&current_critical_section_cpu) = curr_node->cpuid;

	BUG_ON(curr_node->cpuid == smp_processor_id());

	incoming_rsp_ptr = &(curr_node->rsp);
	outgoing_rsp_ptr = this_cpu_ptr(&local_shadow_stack_ptr);

	/*
	 * Make the actual switch, the pushed return address is after this
	 * function call, which we will resume execution at using the switch
	 * in unlock.
	 */
	local_irq_disable();
	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
	local_irq_enable();

	BUG_ON((this_cpu_ptr(&local_shadow_stack)->ptr) -
		       (*this_cpu_ptr(&local_shadow_stack_ptr)) >
	       SIZE_OF_SHADOW_STACK);

	if (lock->locked == _Q_UNLOCKED_OOO_VAL) {
		curr_node->rsp = this_cpu_ptr(&komb_rwlock_nodes[0])->rsp;
		lock->locked = _Q_LOCKED_COMBINER_VAL;
	}

	clear_locked_set_completed(curr_node);
}

#pragma GCC push_options
#pragma GCC optimize("O3")
__attribute__((noipa)) noinline notrace static void
run_combiner(struct orig_qspinlock *lock, struct komb_rwlock_node *curr_node)
{
#ifdef NUMA_AWARE
	struct komb_rwlock_node **local_head, **local_tail;
#endif
	BUG_ON(curr_node == NULL);

	struct komb_rwlock_node *next_node = curr_node->next;
	int counter = 0;

	if (next_node == NULL) {
		set_locked(lock);
		/*
		 * Make this node spin on the locked variable and then it will 
		 * become the combiner.
		 */
		curr_node->locked = false;
		smp_mb();
		return;
	}

	while (curr_node) {
		counter++;

		next_node = get_next_node(curr_node);

		execute_cs(lock, curr_node);

		if (next_node == NULL || next_node->next == NULL ||
		    counter >= komb_batch_size)
			break;

		curr_node = next_node;
	}

#ifdef NUMA_AWARE
	local_head = this_cpu_ptr(&local_queue_head);
	local_tail = this_cpu_ptr(&local_queue_tail);

	if (*local_head != NULL) {
		(*local_tail)->next = next_node;
		next_node = *local_head;
		*local_head = NULL;
		*local_tail = NULL;
	}
#endif

	BUG_ON(next_node == NULL);

	set_locked(lock);
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
__kombrw_spin_lock_longjmp(struct orig_qspinlock *lock, int tail,
			   register struct komb_rwlock_node *curr_node)
{
	register struct komb_rwlock_node *prev_node = NULL, *next_node = NULL;
	struct orig_qspinlock *parent_lock;
	int old_tail, val, j;

	/*
	 * TODO: Make sure these variables are stored on the combiner stack
	 * so that they can be restored later.
	 * These variables are needed when there is a waiter or a combiner
	 * running within a combiner (For same or different lock).
	 */
	uint8_t prev_cs_cpu;
	bool prev_locked_val;
	uint64_t prev_rsp;
#ifdef NUMA_AWARE
	struct komb_rwlock_node *prev_local_queue_head;
	struct komb_rwlock_node *prev_local_queue_tail;
#endif

	old_tail = xchg_tail(lock, tail);

	if (old_tail & _Q_TAIL_MASK) {
		prev_node = decode_tail(old_tail);

		prev_node->next = curr_node;

		while (curr_node->locked)
			cpu_relax();

		if (curr_node->completed) {
			for (j = 7; j >= 0; j--)
				if (this_cpu_ptr(&local_shadow_stack)
					    ->lock_addr[j] != NULL)
					break;
			curr_node->count--;
			if (j >= 0) {
				parent_lock = this_cpu_ptr(&local_shadow_stack)
						      ->lock_addr[j];

				BUG_ON(parent_lock == lock);

				if (parent_lock->locked == _Q_UNLOCKED_OOO_VAL)
					return 1;
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
	set_locked(lock);

	/*
	 * contended path; wait for next if not observed yet, release.
	 */
	while (!next_node) {
		next_node = curr_node->next;
		cpu_relax();
	}

	prev_cs_cpu = *this_cpu_ptr(&current_critical_section_cpu);

	/*
	 * curr_node at the head of the queue. 
	 * Release the node and run_combiner.
	 */
	curr_node->count--;
	prev_locked_val = lock->locked;
	BUG_ON(prev_locked_val >= _Q_LOCKED_COMBINER_VAL);
	lock->locked = _Q_LOCKED_COMBINER_VAL;
	prev_rsp = curr_node->rsp;

#ifdef NUMA_AWARE
	prev_local_queue_head = *this_cpu_ptr(&local_queue_head);
	prev_local_queue_tail = *this_cpu_ptr(&local_queue_tail);

	*this_cpu_ptr(&local_queue_head) = NULL;
	*this_cpu_ptr(&local_queue_tail) = NULL;
#endif

	j = 7;
	for (j = 7; j >= 0; j--)
		if (this_cpu_ptr(&local_shadow_stack)->lock_addr[j] != NULL)
			break;
	j += 1;
	BUG_ON(j >= 8 || j < 0);
	this_cpu_ptr(&local_shadow_stack)->lock_addr[j] = lock;

	run_combiner(lock, next_node);

	BUG_ON(this_cpu_ptr(&local_shadow_stack)->lock_addr[j] != lock);
	this_cpu_ptr(&local_shadow_stack)->lock_addr[j] = NULL;

#ifdef NUMA_AWARE
	*this_cpu_ptr(&local_queue_head) = prev_local_queue_head;
	*this_cpu_ptr(&local_queue_tail) = prev_local_queue_tail;
#endif

	curr_node->rsp = prev_rsp;

	lock->locked = prev_locked_val;

	/*
	 * Set the current_critical_section_cpu variable to this_cpu.
	 */
	*this_cpu_ptr(&current_critical_section_cpu) = prev_cs_cpu;
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
__kombrw_spin_lock_slowpath(struct orig_qspinlock *lock)
{
	struct komb_rwlock_node *curr_node;
	int tail, idx;

	curr_node = this_cpu_ptr(&komb_rwlock_nodes[0]);
	idx = 0;
	tail = encode_tail(smp_processor_id(), idx);

	BUG_ON(idx >= MAX_NODES);

	/*
	 * Initialize curr_node
	 */
	curr_node->locked = true;
	curr_node->completed = false;
	curr_node->next = NULL;
	curr_node->tail = tail;
	curr_node->socket_id = numa_node_id();
	curr_node->cpuid = smp_processor_id();

	return __kombrw_spin_lock_longjmp(lock, tail, curr_node);
}

/*
 * Public API
 */

void komb_rwinit(void)
{
	int i, j;
	for_each_online_cpu (i) {
		void *ptr = vzalloc(SIZE_OF_SHADOW_STACK);
		per_cpu_ptr(&local_shadow_stack, i)->ptr =
			ptr + SIZE_OF_SHADOW_STACK;
		for (j = 0; j < 8; j++)
			per_cpu_ptr(&local_shadow_stack, i)->lock_addr[j] = 0;
		*per_cpu_ptr(&local_shadow_stack_ptr, i) =
			ptr + SIZE_OF_SHADOW_STACK;
		*per_cpu_ptr(&current_critical_section_cpu, i) = i;
	}
}

void komb_rwfree(void)
{
	int i;
	for_each_online_cpu (i) {
		vfree(per_cpu_ptr(&local_shadow_stack, i)->ptr -
		      SIZE_OF_SHADOW_STACK);
	}
}

void komb_rwlock_init(struct komb_rwlock *lock)
{
	atomic_set(&lock->cnts, 0);
	atomic_set(&lock->lock.val, 0);
}

__attribute__((noipa)) noinline notrace static uint64_t
get_shadow_stack_ptr(void)
{
	return this_cpu_ptr(&local_shadow_stack_ptr);
}

__attribute__((noipa)) noinline notrace static struct komb_rwlock_node *
get_komb_rwlock_node(void)
{
	return this_cpu_ptr(&komb_rwlock_nodes[0]);
}

/* 
 * When the following function is called, the compiler in caller frame does
 * this sequence:
 *
 * push caller-saved-regs			(rdi, rsi, rdx, rcx, ...)
 * ...<prepare arguments>...
 * .- push eip+1				(eip + 1 == pop insn)
 * callq |
 * `- jmp kombrw_spin_lock
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
kombrw_spin_lock_slowpath(struct orig_qspinlock *lock)
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
	local_irq_disable();
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
		     : "i"(get_komb_rwlock_node),
		       "i"(offsetof(struct komb_rwlock_node, rsp))
		     : "memory");
	asm volatile("callq %P0\n"
		     "movq (%%rax), %%rsp\n"
		     :
		     : "i"(get_shadow_stack_ptr)
		     : "memory");
	local_irq_enable();

	ret_val = __kombrw_spin_lock_slowpath(lock);

	local_irq_disable();
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
			     : "i"(get_komb_rwlock_node),
			       "i"(offsetof(struct komb_rwlock_node, rsp))
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
	local_irq_enable();
}
#pragma GCC pop_options

__attribute__((noipa)) noinline notrace void
kombrw_spin_lock(struct orig_qspinlock *lock, int state)
{
	u32 val, cnt;

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
	if (val & ~_Q_LOCKED_MASK)
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
	kombrw_spin_lock_slowpath(lock);
}

/* 
 * No IPA is even more relevant here, because even if assembly is removed,
 * compiler shouldn't assume which caller-saved registers are in use inside
 * the function. When we jump from the spin loop to the return address after
 * the unlock function, we have caller-saved registers that are completely
 * inconsistent, because we only restored the callee-saved registers.
 */
__attribute__((noipa)) noinline notrace void
kombrw_spin_unlock(struct orig_qspinlock *lock)
{
	int from_cpuid = *this_cpu_ptr(&current_critical_section_cpu);
	void *incoming_rsp_ptr, *outgoing_rsp_ptr;

	int j, max_idx, my_idx;

	uint64_t temp_lock_addr;

	if (lock->locked == 1) {
		lock->locked = false;
		return;
	}

	if (lock->locked != _Q_LOCKED_COMBINER_VAL)
		BUG_ON(lock->locked == _Q_LOCKED_COMBINER_VAL);

	j = 0;
	max_idx = -1;
	my_idx = -1;

	for (j = 0; j < 8; j++) {
		temp_lock_addr =
			this_cpu_ptr(&local_shadow_stack)->lock_addr[j];
		if (temp_lock_addr != NULL)
			max_idx = j;
		if (temp_lock_addr == lock)
			my_idx = j;
	}

	if (max_idx < 0) {
		BUG_ON(true);
	}

	if (my_idx < max_idx) {
		lock->locked = _Q_UNLOCKED_OOO_VAL;
		return;
	}

	/*
	 * Current cpu running waiter critical section.
	 */
	incoming_rsp_ptr = this_cpu_ptr(&local_shadow_stack_ptr);

	/*
	 * Komb node still active here, because cpu (from_cpuid) still spinning.
	 */
	outgoing_rsp_ptr =
		&((per_cpu_ptr(&komb_rwlock_nodes[0], from_cpuid)->rsp));

	local_irq_disable();
	komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
	local_irq_enable();
	return;
}

static inline void komb_read_lock_slowpath(struct komb_rwlock *lock)
{
	if (unlikely(in_interrupt())) {
		atomic_cond_read_acquire(
			&lock->cnts,
			!(VAL & (_KOMB_W_LOCKED | _KOMB_W_LOCKED_SLOW)));
		return;
	}

	atomic_sub(_KOMB_R_BIAS, &lock->cnts);

	kombrw_spin_lock(&lock->lock, READ_STATE);
	atomic_add(_KOMB_R_BIAS, &lock->cnts);

	atomic_cond_read_acquire(
		&lock->cnts, !(VAL & (_KOMB_W_LOCKED | _KOMB_W_LOCKED_SLOW)));

	kombrw_spin_unlock(&lock->lock);
}

static inline void komb_write_lock_slowpath(struct komb_rwlock *lock)
{
	kombrw_spin_lock(&lock->lock, WRITE_STATE);

	if (!atomic_read(&lock->cnts) &&
	    (atomic_cmpxchg_acquire(&lock->cnts, 0, _KOMB_W_LOCKED_SLOW) == 0))
		goto unlock;

	atomic_add(_KOMB_W_WAITING, &lock->cnts);

	do {
		atomic_cond_read_acquire(&lock->cnts, VAL == _KOMB_W_WAITING);
	} while (atomic_cmpxchg_relaxed(&lock->cnts, _KOMB_W_WAITING,
					_KOMB_W_LOCKED_SLOW) !=
		 _KOMB_W_WAITING);

unlock:
	return;
	// kombrw_spin_unlock(&lock->lock);
}

void komb_write_lock(struct komb_rwlock *lock)
{
	if (atomic_cmpxchg_acquire(&lock->cnts, 0, _KOMB_W_LOCKED) == 0)
		return;
	komb_write_lock_slowpath(lock);
}

/* 
 * No IPA is even more relevant here, because even if assembly is removed,
 * compiler shouldn't assume which caller-saved registers are in use inside
 * the function. When we jump from the spin loop to the return address after
 * the unlock function, we have caller-saved registers that are completely
 * inconsistent, because we only restored the callee-saved registers.
 */
void komb_write_unlock(struct komb_rwlock *lock)
{
	u32 val;

	val = smp_load_acquire(&lock->wlocked);

	smp_store_release(&lock->wlocked, 0);
	if (likely(val == _KOMB_W_LOCKED))
		return;

	kombrw_spin_unlock(&lock->lock);
}

void komb_read_lock(struct komb_rwlock *lock)
{
	u32 cnts;

	cnts = atomic_add_return_acquire(_KOMB_R_BIAS, &lock->cnts);
	if (likely(!(cnts & _KOMB_W_WMASK)))
		return;

	komb_read_lock_slowpath(lock);
}

void komb_read_unlock(struct komb_rwlock *lock)
{
	(void)atomic_sub_return_release(_KOMB_R_BIAS, &lock->cnts);
}
