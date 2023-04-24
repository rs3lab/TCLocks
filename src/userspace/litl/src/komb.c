#include <assert.h>
#include <errno.h>
#include <papi.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "interpose.h"
#include "utils.h"
#include "waiting_policy.h"
#include <combiner.h>
#include <komb.h>
#include <combiner.h>

#pragma GCC push_options
#pragma GCC optimize("O3")

//#define WAITER_DEBUG

/* debugging */
#ifdef WAITER_DEBUG
typedef enum {
    RED,
    GREEN,
    BLUE,
    MAGENTA,
    YELLOW,
    CYAN,
    END,
} color_num;

static char colors[END][8] = {
    "\x1B[31m", "\x1B[32m", "\x1B[34m", "\x1B[35m", "\x1b[33m", "\x1b[36m",
};
static unsigned long counter = 0;

#define dprintf(__fmt, ...)                                                    \
    do {                                                                       \
        smp_faa(&counter, 1);                                                  \
        fprintf(stderr, "%s [DBG:%010lu: %d (%s: %d)]: " __fmt,                \
                colors[cur_thread_id % END], counter, cur_thread_id, __func__, \
                __LINE__, ##__VA_ARGS__);                                      \
    } while (0);
#define BUG_ON(v) assert(!(v))
#else
#define dprintf(__fmt, ...)                                                    \
    do {                                                                       \
    } while (0)
#define BUG_ON(v)                                                             \
    do {                                                                       \
    } while (0)
#endif

#define __scalar_type_to_expr_cases(type)                                      \
    unsigned type : (unsigned type)0, signed type : (signed type)0

#define __unqual_scalar_typeof(x)                                              \
    typeof(_Generic((x), char                                                  \
                    : (char)0, __scalar_type_to_expr_cases(char),              \
                      __scalar_type_to_expr_cases(short),                      \
                      __scalar_type_to_expr_cases(int),                        \
                      __scalar_type_to_expr_cases(long),                       \
                      __scalar_type_to_expr_cases(long long), default          \
                    : (x)))

#define smp_cond_load_relaxed(ptr, cond_expr)                                  \
    ({                                                                         \
        typeof(ptr) __PTR = (ptr);                                             \
        __unqual_scalar_typeof(*ptr) VAL;                                      \
        for (;;) {                                                             \
            VAL = READ_ONCE(*__PTR);                                           \
            if (cond_expr)                                                     \
                break;                                                         \
            CPU_PAUSE();                                                       \
        }                                                                      \
        (typeof(*ptr))VAL;                                                     \
    })

extern __thread unsigned int cur_thread_id;

#define eprintf(__fmt, ...)                                                    \
    do {                                                                       \
        fprintf(stderr, "%s [%d (%s: %d)]: " __fmt,                            \
                colors[cur_thread_id % END], cur_thread_id, __func__,          \
                __LINE__, ##__VA_ARGS__);                                      \
    } while (0);

static inline int current_numa_node() {
    unsigned long a, d, c;
    int core;
    __asm__ volatile("rdtscp" : "=a"(a), "=d"(d), "=c"(c));
    core = c & 0xFFF;
    return core / (CPU_NUMBER / NUMA_NODES);
}

#define false 0
#define true 1

#define atomic_andnot(val, ptr) __sync_fetch_and_and((ptr), ~(val));
#define atomic_fetch_or_acquire(val, ptr) __sync_fetch_and_or((ptr), (val));

static long komb_batch_size = 262144;

static inline void smp_wmb(void) {
    __asm __volatile("sfence" ::: "memory");
}

static inline void smp_mb(void) {
    __asm __volatile("mfence" ::: "memory");
}

_Thread_local komb_node_t *volatile local_queue_head;
_Thread_local komb_node_t *volatile local_queue_tail;
_Thread_local volatile komb_node_t my_local_node;
_Thread_local komb_mutex_t *volatile lock_addr[8];

_Thread_local volatile char shadow_stack_ptr[8192];

_Thread_local void *volatile local_shadow_stack_ptr;

_Thread_local komb_node_t *volatile komb_prev_node;
_Thread_local komb_node_t *volatile komb_curr_node;
_Thread_local komb_node_t *volatile komb_next_node;
_Thread_local volatile long counter_val;

/*
 * incoming_rsp_ptr -> rdi
 * outgoing_rsp_ptr -> rsi
 *
 * Assume IRQ is disabled. Will enable when returning.
 */
__attribute__((noipa, noinline)) void
komb_context_switch(void *incoming_rsp_ptr, void *outgoing_rsp_ptr) {
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

static __always_inline void clear_locked_set_completed(komb_node_t *node) {
    WRITE_ONCE(node->locked_completed, 1);
}

/**
 * set_locked - Set the lock bit and own the lock
 * @lock: Pointer to queued spinlock structure
 *
 * *,*,0 -> *,0,1
 */
static __always_inline void set_locked(komb_mutex_t *lock) {
    WRITE_ONCE(lock->locked, _Q_LOCKED_VAL);
}

static __always_inline void check_and_set_combiner(komb_mutex_t *lock) {
    //BUG_ON(lock->locked == _Q_LOCKED_VAL);

    while (true) {
        smp_cond_load_relaxed(&lock->locked, !(VAL));

        //BUG_ON(lock->locked != 0);

        if (smp_cas(&lock->locked, 0, _Q_LOCKED_COMBINER_VAL) == 0)
            return;
    }
}

__always_inline static void add_to_local_queue(komb_node_t *node) {

    if (local_queue_head == NULL) {
        local_queue_head = node;
        local_queue_tail = node;
    } else {
        local_queue_tail->next = node;
        local_queue_tail       = node;
    }
}

__always_inline static komb_node_t *get_next_node(komb_node_t *my_node) {
    komb_node_t *curr_node, *next_node;

    curr_node = my_node;
    next_node = curr_node->next;

    while (true) {
        if (next_node == NULL || next_node->next == NULL)
            goto next_node_null;

        if (next_node->socket_id == current_numa_node()) {
            void *rsp_ptr = next_node->rsp;
            PREFETCHW((rsp_ptr));
            PREFETCHW((rsp_ptr + 64));
            PREFETCHW((rsp_ptr + 128));
            PREFETCHW((rsp_ptr + 192));
            PREFETCHW((rsp_ptr + 256));
            PREFETCHW((rsp_ptr + 320));

            PREFETCH((next_node->next));

            return next_node;
        }

        add_to_local_queue(next_node);
        curr_node = next_node;
        next_node = curr_node->next;
    }

next_node_null:
    return next_node;
}

__attribute__((noipa, noinline)) static void
execute_cs(komb_mutex_t *lock, komb_node_t *curr_node) {
    void *incoming_rsp_ptr, *outgoing_rsp_ptr;

    komb_curr_node = curr_node;

    BUG_ON(curr_node->cpuid == cur_thread_id);
    /*if ((ptr->ptr - (ptr->local_shadow_stack_ptr)) > SIZE_OF_SHADOW_STACK) {
        dprintf(KERN_ALERT "%d %px %px\n", cur_thread_id, ptr->ptr,
                ptr->local_shadow_stack_ptr);
        BUG_ON(true);
    }*/

    incoming_rsp_ptr = &(curr_node->rsp);
    outgoing_rsp_ptr = (void *)&(local_shadow_stack_ptr);

    komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);

    /*if ((ptr->ptr - (ptr->local_shadow_stack_ptr)) > SIZE_OF_SHADOW_STACK) {
        dprintf(KERN_ALERT "%d %px %px\n", cur_thread_id, ptr->ptr,
                ptr->local_shadow_stack_ptr);
        BUG_ON(true);
    }*/
    // BUG_ON(ptr->ptr - (ptr->local_shadow_stack_ptr) > SIZE_OF_SHADOW_STACK);
}

__attribute__((noipa, noinline)) static void
run_combiner(komb_mutex_t *lock, komb_node_t *curr_node) {
    BUG_ON(curr_node == NULL);
    komb_node_t *next_node = curr_node->next;

    if (next_node == NULL) {
        set_locked(lock);
        curr_node->locked = false;
        smp_mb();
        return;
    }

    counter_val = 0;

    dprintf("Combiner %d giving control to %d\n", cur_thread_id,
            curr_node->cpuid);

    execute_cs(lock, curr_node);

    dprintf("Combiner got the control back: %d counter: %ld last_waiter: %d\n",
            cur_thread_id, counter_val, komb_curr_node->cpuid);

    if (komb_prev_node != NULL) {
        clear_locked_set_completed(komb_prev_node);
        komb_prev_node = NULL;
    }

    next_node = komb_next_node;

    if (local_queue_head != NULL) {
        local_queue_tail->next = next_node;
        next_node              = local_queue_head;
        local_queue_head       = NULL;
        local_queue_tail       = NULL;
    }

    BUG_ON(next_node == NULL);

    dprintf("After combiner %d, next node: %d\n", cur_thread_id,
            next_node->cpuid);

    set_locked(lock);
    komb_curr_node    = NULL;
    next_node->locked = false;
}

__attribute__((noipa, noinline)) static int
__komb_spin_lock_longjmp(komb_mutex_t *lock, komb_node_t *curr_node) {
    register komb_node_t *prev_node = NULL, *next_node = NULL;

    prev_node = smp_swap(&lock->tail, curr_node);

    if (prev_node) {

        WRITE_ONCE(prev_node->next, curr_node);

        smp_cond_load_relaxed(&curr_node->locked, !(VAL));

        if (curr_node->completed) {
            int j = 7;
            for (j = 7; j >= 0; j--)
                if (lock_addr[j] != NULL)
                    break;

            curr_node->count--;

            BUG_ON(lock_addr[j] == lock);
            return 0;
        }
    }

    check_and_set_combiner(lock);

    if (lock->tail == curr_node &&
        smp_cas(&lock->tail, curr_node, NULL) == curr_node) {
        set_locked(lock);
        goto release; /* No contention */
    }

    smp_cond_load_relaxed(&curr_node->next, (VAL));
    next_node = curr_node->next;
    BUG_ON(next_node == NULL);

    komb_curr_node   = NULL;
    komb_prev_node   = NULL;
    komb_next_node   = NULL;
    counter_val      = 0;
    local_queue_head = NULL;
    local_queue_tail = NULL;

    curr_node->count--;
    lock->locked = _Q_LOCKED_COMBINER_VAL;

    int j;
    for (j = 7; j >= 0; j--)
        if (lock_addr[j] != NULL)
            break;
    j += 1;

    if (j >= 8 || j < 0)
        BUG_ON(true);
    else
        lock_addr[j] = lock;

    run_combiner(lock, next_node);

    BUG_ON(lock_addr[j] != lock);

    lock_addr[j] = NULL;

    return 0;

release:
    /*
     * release the node
     */
    curr_node->count--;
    return 0;
}

__attribute__((noipa, noinline)) static int
__komb_spin_lock_slowpath(komb_mutex_t *lock) {
    komb_node_t *curr_node;

    curr_node = (void *)&my_local_node;

    /*
     * Initialize curr_node
     */
    curr_node->locked    = true;
    curr_node->completed = false;
    curr_node->next      = NULL;
    curr_node->socket_id = current_numa_node();
    curr_node->cpuid     = cur_thread_id;
    curr_node->lock      = lock;

    return __komb_spin_lock_longjmp(lock, curr_node);
}


__attribute__((noipa, noinline)) static void *get_shadow_stack_ptr(void) {
    if(local_shadow_stack_ptr == 0)
	    local_shadow_stack_ptr = (void *)shadow_stack_ptr + (8192 - 64);
    return (void *)&local_shadow_stack_ptr;
}

__attribute__((noipa, noinline)) static komb_node_t *get_komb_node(void) {
    return (void *)&my_local_node;
}

__attribute__((noipa, noinline)) void
komb_spin_lock_slowpath(komb_mutex_t *lock) {
    asm volatile("add $0x8,%%rsp\n" ::: "memory");
    asm volatile("pushq %%rbp\n"
                 "pushq %%rbx\n"
                 "pushq %%r12\n"
                 "pushq %%r13\n"
                 "pushq %%r14\n"
                 "pushq %%r15\n"
		 "pushq %%rdi\n"
                 :
                 :
                 : "memory");
    asm volatile("callq %P0\n"
		 "popq %%rdi\n"
                 "movq %%rsp, %c1(%%rax)\n"
		 "pushq %%rdi\n"
                 :
                 : "i"(get_komb_node), "i"(offsetof(komb_node_t, rsp))
                 : "memory");

    asm volatile("callq %P0\n"
		 "popq %%rdi\n"
                 "movq (%%rax), %%rsp\n"
                 :
                 : "i"(get_shadow_stack_ptr)
                 : "memory");

    __komb_spin_lock_slowpath(lock);

    asm volatile("callq %P0\n"
                 "movq %%rsp, (%%rax)\n"
                 :
                 : "i"(get_shadow_stack_ptr)
                 : "memory");

    asm volatile("callq %P0\n"
                 "movq %c1(%%rax), %%rsp\n"
                 :
                 : "i"(get_komb_node), "i"(offsetof(komb_node_t, rsp))
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

/* Interpose */
komb_mutex_t *komb_mutex_create(const pthread_mutexattr_t *attr) {
    komb_mutex_t *impl =
        (komb_mutex_t *)alloc_cache_align(sizeof(komb_mutex_t));
    impl->tail   = NULL;
    impl->locked = 0;
#if COND_VAR
    REAL(pthread_mutex_init)(&impl->posix_lock, attr);
    DEBUG("Mutex init lock=%p posix_lock=%p\n", impl, &impl->posix_lock);
#endif

    barrier();
    return impl;
}

static int __komb_mutex_lock(komb_mutex_t *lock, komb_node_t *me) {

    if (smp_cas(&lock->locked, 0, _Q_LOCKED_VAL) == 0) {
        goto release;
    }

    komb_spin_lock_slowpath(lock);

    if (komb_curr_node != NULL) {
        if (komb_curr_node->lock == lock) {
            BUG_ON(lock->locked != _Q_LOCKED_COMBINER_VAL);
            komb_next_node = get_next_node(komb_curr_node);
        }

        if (komb_prev_node != NULL) {
            BUG_ON(komb_prev_node->lock != lock);
            dprintf("Waking up prev waiter: %d\n", komb_prev_node->cpuid);
            clear_locked_set_completed(komb_prev_node);
            komb_prev_node = NULL;
        }
    }

release:
    return 0;
}

int komb_mutex_lock(komb_mutex_t *impl, komb_node_t *UNUSED(me)) {
    komb_node_t node;
    int ret = __komb_mutex_lock(impl, &node);
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

int komb_mutex_trylock(komb_mutex_t *impl, komb_node_t *UNUSED(me)) {

    if ((smp_cas(&impl->locked, 0, _Q_LOCKED_VAL) == 0)) {
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

__attribute__((noipa, noinline)) void __komb_mutex_unlock(komb_mutex_t *lock) {
    void *incoming_rsp_ptr, *outgoing_rsp_ptr;
    int j, max_idx, my_idx;
    komb_mutex_t *temp_lock_addr;

    (void)max_idx;
    j       = 0;
    max_idx = -1;
    my_idx  = -1;

    for (j = 0; j < 8; j++) {
        temp_lock_addr = lock_addr[j];
        if (temp_lock_addr != NULL)
            max_idx = j;
        if (temp_lock_addr == lock)
            my_idx = j;
        if (temp_lock_addr == NULL)
            break;
    }

    if (my_idx == -1) {
        if (lock->locked == _Q_LOCKED_VAL)
            lock->locked = false;
        else
            BUG_ON(true);
        return;
    }

    BUG_ON(lock->locked != _Q_LOCKED_COMBINER_VAL);
    BUG_ON(max_idx < 0);

    BUG_ON(my_idx != max_idx);

    if (komb_next_node == NULL || komb_next_node->next == NULL ||
        counter_val >= komb_batch_size) {
        incoming_rsp_ptr = (void *)&(local_shadow_stack_ptr);
        komb_prev_node   = komb_curr_node;
        komb_curr_node   = NULL;
    } else {
        komb_prev_node   = komb_curr_node;
        komb_curr_node   = komb_next_node;
        incoming_rsp_ptr = &(komb_next_node->rsp);
        counter_val += 1;
        dprintf("Jumping to the next waiter: %d\n", komb_next_node->cpuid);
    }

    outgoing_rsp_ptr = &(komb_prev_node->rsp);

    komb_context_switch(incoming_rsp_ptr, outgoing_rsp_ptr);
    return;
}

void komb_mutex_unlock(komb_mutex_t *impl, komb_node_t *UNUSED(me)) {
#if COND_VAR
    DEBUG_PTHREAD("[%d] Unlock posix=%p\n", cur_thread_id, &impl->posix_lock);
    assert(REAL(pthread_mutex_unlock)(&impl->posix_lock) == 0);
#endif
    __komb_mutex_unlock(impl);
}

int komb_mutex_destroy(komb_mutex_t *lock) {
#if COND_VAR
    REAL(pthread_mutex_destroy)(&lock->posix_lock);
#endif
    // free(lock);
    // lock = NULL;

    return 0;
}

int komb_cond_init(komb_cond_t *cond, const pthread_condattr_t *attr) {
#if COND_VAR
    return REAL(pthread_cond_init)(cond, attr);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int komb_cond_timedwait(komb_cond_t *cond, komb_mutex_t *lock,
                        komb_node_t *UNUSED(me), const struct timespec *ts) {
#if COND_VAR
    int res;
    komb_node_t node;

    __komb_mutex_unlock(lock);
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

    komb_mutex_lock(lock, &node);

    return res;
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int komb_cond_wait(komb_cond_t *cond, komb_mutex_t *lock, komb_node_t *me) {
    return komb_cond_timedwait(cond, lock, me, 0);
}

int komb_cond_signal(komb_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_signal)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int komb_cond_broadcast(komb_cond_t *cond) {
#if COND_VAR
    DEBUG("[%d] Broadcast cond=%p\n", cur_thread_id, cond);
    return REAL(pthread_cond_broadcast)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int komb_cond_destroy(komb_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_destroy)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

void komb_thread_start(void) {
    local_queue_head = NULL;
    local_queue_tail = NULL;
    for (int i = 0; i < 8; i++)
        lock_addr[i] = NULL;

    local_shadow_stack_ptr = (void *)shadow_stack_ptr + (8192 - 64);

    komb_prev_node = NULL;
    komb_curr_node = NULL;
    komb_next_node = NULL;
    counter_val    = 0;
}

void komb_thread_exit(void) {
}

void komb_application_init(void) {
}

void komb_application_exit(void) {
}
void komb_init_context(lock_mutex_t *UNUSED(impl),
                       lock_context_t *UNUSED(context), int UNUSED(number)) {
}

#pragma GCC pop_options
