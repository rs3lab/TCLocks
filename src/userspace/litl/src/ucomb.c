#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <assert.h>
#include <ucomb.h>

#include "waiting_policy.h"
#include "interpose.h"
#include "utils.h"

extern __thread unsigned int cur_thread_id;

static inline int current_numa_node() {
    unsigned long a, d, c;
    int core;
    __asm__ volatile("rdtscp" : "=a"(a), "=d"(d), "=c"(c));
    core = c & 0xFFF;
    return core / (CPU_NUMBER / NUMA_NODES);
}

ucomb_mutex_t *ucomb_mutex_create(const pthread_mutexattr_t *attr) {
    ucomb_mutex_t *impl = (ucomb_mutex_t *)alloc_cache_align(sizeof(ucomb_mutex_t));
    impl->tail        = 0;
#if COND_VAR
    REAL(pthread_mutex_init)(&impl->posix_lock, /*&errattr */ attr);
    DEBUG("Mutex init lock=%p posix_lock=%p\n", impl, &impl->posix_lock);
#endif

    return impl;
}

#pragma GCC push_options
#pragma GCC optimize("O3")

_Thread_local ucomb_node_t *numa_queue_head;
_Thread_local ucomb_node_t *numa_queue_tail;
_Thread_local ucomb_node_t curr_switcher_;
_Thread_local bool in_combiner_context_;
_Thread_local struct ContextSwitchFrame *prev_thread_frame_;
_Thread_local struct ContextSwitchFrame *curr_node_frame_;
_Thread_local struct ContextSwitchFrame curr_thread_frame_;

__attribute__((always_inline))
static inline void ContextSwitchSetJump(struct ContextSwitchFrame *frame)
{
	asm volatile ("movq %%rsp, %0\n"
		      "movq %%rbp, %1\n"
		      "movq %%rbx, %2\n"
		      "movq %%r12, %3\n"
		      "movq %%r13, %4\n"
		      "movq %%r14, %5\n"
		      "movq %%r15, %6\n"
		      : "=m" (frame->rsp),
			"=m" (frame->rbp),
			"=m" (frame->rbx),
			"=m" (frame->r12),
			"=m" (frame->r13),
			"=m" (frame->r14),
			"=m" (frame->r15)
		      : : "memory");
}

__attribute__((always_inline))
static inline void ContextSwitchLongJump(const struct ContextSwitchFrame *frame)
{
	asm volatile ("movq %0, %%r15\n"
		      "movq %1, %%r14\n"
		      "movq %2, %%r13\n"
		      "movq %3, %%r12\n"
		      "movq %4, %%rbx\n"
		      "movq %5, %%rbp\n"
		      "movq %6, %%rsp\n"
		      : : "m" (frame->r15),
			  "m" (frame->r14),
			  "m" (frame->r13),
			  "m" (frame->r12),
			  "m" (frame->rbx),
			  "m" (frame->rbp),
			  "m" (frame->rsp)
		      : "memory");
}

__attribute__((noinline,noipa))
static void ContextSwitch(struct ContextSwitchFrame *prev, struct ContextSwitchFrame *next) {
	if (prev)
		ContextSwitchSetJump(prev);
	ContextSwitchLongJump(next);
}

static void PushToLocalQueue(ucomb_node_t *switcher)
{
	if (!numa_queue_head) {
		numa_queue_head = switcher;
		numa_queue_tail = switcher;
	} else {
		atomic_store_explicit(&numa_queue_tail->next, switcher, memory_order_relaxed);
		numa_queue_tail = switcher;
	}
}

static ucomb_node_t *GetNextWaiter(ucomb_node_t *waiter)
{
	int numa_node_id = current_numa_node();
	ucomb_node_t *next_waiter;

	assert(numa_node_id >= 0);
	assert(waiter);

	next_waiter = atomic_load_explicit(&waiter->next, memory_order_relaxed);
	for (;;) {
		if (next_waiter == NULL ||
		    !atomic_load_explicit(&next_waiter->next, memory_order_relaxed))
			return next_waiter;

		if (next_waiter->numa_node_id == numa_node_id)
			return next_waiter;

		PushToLocalQueue(next_waiter);
		waiter = next_waiter;
		next_waiter = atomic_load_explicit(&next_waiter->next, memory_order_relaxed);
		atomic_store_explicit(&waiter->next, NULL, memory_order_relaxed);
	}
}

// This function in no way can use the stack, or manipulate stack pointer
__attribute__((noinline,noipa))
void UcombSpinLockPublishAndWait(ucomb_node_t *switcher, ucomb_node_t *owner) {
	ContextSwitchSetJump(&switcher->frame_);
	atomic_store_explicit(&owner->next, switcher, memory_order_seq_cst);
	while (switcher->locked)
		asm volatile ("rep nop\n" : : : "memory");
	atomic_thread_fence(memory_order_acquire);
	if (switcher->completed)
		ContextSwitch(NULL, &switcher->frame_);
}

__attribute__((noinline,noipa))
static void UcombSpinUnlock(ucomb_mutex_t *mtx, ucomb_node_t *switcher_) {
	ucomb_node_t *waiter, *expected = &curr_switcher_;

	if (in_combiner_context_) {
		ContextSwitch(curr_node_frame_, prev_thread_frame_);
		if (!curr_switcher_.next_owner)
			return;
	}

	// Pair with seq_cst release of owner->next_ptr_ store in UniqueLock::SpinLock
	waiter = atomic_load_explicit(&curr_switcher_.next, memory_order_acquire);
	if (!waiter) {
		if (atomic_compare_exchange_strong_explicit(&mtx->tail, &expected, NULL, memory_order_release, memory_order_relaxed))
			return;
		while (!(waiter = atomic_load_explicit(&curr_switcher_.next, memory_order_relaxed)))
			asm volatile("rep nop\n" : : : "memory");
		// Pair with seq_cst release of owner->next_ptr_ store in UniqueLock::SpinLock
		atomic_thread_fence(memory_order_acquire);
	}

	const int kMaxBatch = 4096;
	volatile int combined = 0;

	numa_queue_head = NULL;
	numa_queue_tail = NULL;
	for (;;) {
		ucomb_node_t *next_waiter;

		if (combined == kMaxBatch || !waiter ||
		    !atomic_load_explicit(&waiter->next, memory_order_relaxed))
			break;
		assert(waiter->locked);
		assert(!waiter->completed);

		in_combiner_context_ = true;
		prev_thread_frame_ = &curr_thread_frame_;
		curr_node_frame_ = &waiter->frame_;
		ContextSwitch(prev_thread_frame_, curr_node_frame_);
		in_combiner_context_ = false;

		next_waiter = GetNextWaiter(waiter);
		waiter->completed = true;
		atomic_thread_fence(memory_order_release);
		waiter->locked = false;
		waiter = next_waiter;

		combined++;
	}

	if (numa_queue_head) {
		atomic_store_explicit(&numa_queue_tail->next, waiter, memory_order_relaxed);
		waiter = numa_queue_head;
		numa_queue_head = NULL;
		numa_queue_tail = NULL;
	}

	assert(waiter->locked);
	assert(!waiter->completed);
	waiter->next_owner = true;
	atomic_thread_fence(memory_order_release);
	waiter->locked = false;
}

#pragma GCC pop_options

static int __ucomb_mutex_lock(ucomb_mutex_t *impl, ucomb_node_t *me) {
    ucomb_node_t *tail;

    me = &curr_switcher_;

    assert(me != NULL);

    me->next = NULL;
    me->locked = true;
    me->completed = false;
    me->next_owner = false;
    me->numa_node_id = current_numa_node();

    // The atomic instruction is needed when two threads try to put themselves
    // at the tail of the list at the same time
    tail = atomic_exchange(&impl->tail, me);

    /* No one there? */
    if (!tail) {
        DEBUG("[%d] (1) Locking lock=%p tail=%p me=%p\n", cur_thread_id, impl,
              impl->tail, me);
        return 0;
    }

    UcombSpinLockPublishAndWait(me, tail);

    DEBUG("[%d] (2) Locking lock=%p tail=%p me=%p\n", cur_thread_id, impl,
          impl->tail, me);
    return 0;
}

int ucomb_mutex_lock(ucomb_mutex_t *impl, ucomb_node_t *me) {
    int ret = __ucomb_mutex_lock(impl, me);
    assert(ret == 0);
#if COND_VAR
    if (ret == 0) {
        DEBUG_PTHREAD("[%d] Lock posix=%p\n", cur_thread_id, &impl->posix_lock);
        assert(REAL(pthread_mutex_lock)(&impl->posix_lock) == 0);
    }
#endif
    DEBUG("[%d] Lock acquired posix=%p\n", cur_thread_id, &impl->posix_lock);
    return ret;
}

int ucomb_mutex_trylock(ucomb_mutex_t *impl, ucomb_node_t *me) {
    ucomb_node_t *tail;

    me = &curr_switcher_;
    assert(me != NULL);

    me->next = NULL;
    me->locked = true;
    me->completed = false;
    me->next_owner = false;
    me->numa_node_id = current_numa_node();

    // The trylock is a cmp&swap, where the thread enqueue itself to the end of
    // the list only if there are nobody at the tail
    tail = __sync_val_compare_and_swap(&impl->tail, 0, me);

    /* No one was there - can quickly return */
    if (!tail) {
        DEBUG("[%d] TryLocking lock=%p tail=%p me=%p\n", cur_thread_id, impl,
              impl->tail, me);
#if COND_VAR
        DEBUG_PTHREAD("[%d] Lock posix=%p\n", cur_thread_id, &impl->posix_lock);
        int ret = 0;
        while ((ret = REAL(pthread_mutex_trylock)(&impl->posix_lock)) == EBUSY)
            ;
        assert(ret == 0);
#endif
        return 0;
    }

    return EBUSY;
}

#define THRESHOLD (0xffff)
#ifndef UNLOCK_COUNT_THRESHOLD
#define UNLOCK_COUNT_THRESHOLD 1024
#endif

static void __ucomb_mutex_unlock(ucomb_mutex_t *impl, ucomb_node_t *me) {
    DEBUG("[%d] Unlocking lock=%p tail=%p me=%p\n", cur_thread_id, impl,
          impl->tail, me);
    me = &curr_switcher_;
    UcombSpinUnlock(impl, me);
}

void ucomb_mutex_unlock(ucomb_mutex_t *impl, ucomb_node_t *me) {
#if COND_VAR
    DEBUG_PTHREAD("[%d] Unlock posix=%p\n", cur_thread_id, &impl->posix_lock);
    assert(REAL(pthread_mutex_unlock)(&impl->posix_lock) == 0);
#endif
    __ucomb_mutex_unlock(impl, me);
}

int ucomb_mutex_destroy(ucomb_mutex_t *lock) {
#if COND_VAR
    REAL(pthread_mutex_destroy)(&lock->posix_lock);
#endif
    free(lock);
    lock = NULL;

    return 0;
}

int ucomb_cond_init(ucomb_cond_t *cond, const pthread_condattr_t *attr) {
#if COND_VAR
    return REAL(pthread_cond_init)(cond, attr);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ucomb_cond_timedwait(ucomb_cond_t *cond, ucomb_mutex_t *lock, ucomb_node_t *me,
                       const struct timespec *ts) {
#if COND_VAR
    int res;

    __ucomb_mutex_unlock(lock, me);
    DEBUG("[%d] Sleep cond=%p lock=%p posix_lock=%p\n", cur_thread_id, cond,
          lock, &(lock->posix_lock));
    DEBUG_PTHREAD("[%d] Cond posix = %p lock = %p\n", cur_thread_id, cond,
                  &lock->posix_lock);

    if (ts)
        res = REAL(pthread_cond_timedwait)(cond, &lock->posix_lock, ts);
    else
        res = REAL(pthread_cond_wait)(cond, &lock->posix_lock);

    if (res != 0 && res != ETIMEDOUT) {
        fprintf(stderr, "Error on cond_{timed,}wait %d\n", res);
        assert(0);
    }

    int ret = 0;
    if ((ret = REAL(pthread_mutex_unlock)(&lock->posix_lock)) != 0) {
        fprintf(stderr, "Error on mutex_unlock %d\n", ret == EPERM);
        assert(0);
    }

    ucomb_mutex_lock(lock, me);

    return res;
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ucomb_cond_wait(ucomb_cond_t *cond, ucomb_mutex_t *lock, ucomb_node_t *me) {
    return ucomb_cond_timedwait(cond, lock, me, 0);
}

int ucomb_cond_signal(ucomb_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_signal)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ucomb_cond_broadcast(ucomb_cond_t *cond) {
#if COND_VAR
    DEBUG("[%d] Broadcast cond=%p\n", cur_thread_id, cond);
    return REAL(pthread_cond_broadcast)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ucomb_cond_destroy(ucomb_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_destroy)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

void ucomb_thread_start(void) {
}

void ucomb_thread_exit(void) {
}

void ucomb_application_init(void) {
}

void ucomb_application_exit(void) {
}
void ucomb_init_context(lock_mutex_t *UNUSED(impl),
                      lock_context_t *UNUSED(context), int UNUSED(number)) {
}
