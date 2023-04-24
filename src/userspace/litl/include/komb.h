#ifndef __KOMB_H__
#define __KOMB_H__

#include <string.h>

#include "padding.h"
#define LOCK_ALGORITHM "KOMB"
#define NEED_CONTEXT 0
#define SUPPORT_WAITING 1

/* Arch utility */
static inline void smp_rmb(void) {
    __asm __volatile("lfence" ::: "memory");
}

static inline void smp_cmb(void) {
    __asm __volatile("" ::: "memory");
}

#define barrier() smp_cmb()

static inline void __write_once_size(volatile void *p, void *res, int size) {
    switch (size) {
    case 1:
        *(volatile uint8_t *)p = *(uint8_t *)res;
        break;
    case 2:
        *(volatile uint16_t *)p = *(uint16_t *)res;
        break;
    case 4:
        *(volatile uint32_t *)p = *(uint32_t *)res;
        break;
    case 8:
        *(volatile uint64_t *)p = *(uint64_t *)res;
        break;
    default:
        barrier();
        memcpy((void *)p, (const void *)res, size);
        barrier();
    }
}

static inline void __read_once_size(volatile void *p, void *res, int size) {
    switch (size) {
    case 1:
        *(uint8_t *)res = *(volatile uint8_t *)p;
        break;
    case 2:
        *(uint16_t *)res = *(volatile uint16_t *)p;
        break;
    case 4:
        *(uint32_t *)res = *(volatile uint32_t *)p;
        break;
    case 8:
        *(uint64_t *)res = *(volatile uint64_t *)p;
        break;
    default:
        barrier();
        memcpy((void *)res, (const void *)p, size);
        barrier();
    }
}

#define WRITE_ONCE(x, val)                                                     \
    ({                                                                         \
        union {                                                                \
            typeof(x) __val;                                                   \
            char __c[1];                                                       \
        } __u = {.__val = (typeof(x))(val)};                                   \
        __write_once_size(&(x), __u.__c, sizeof(x));                           \
        __u.__val;                                                             \
    })

#define READ_ONCE(x)                                                           \
    ({                                                                         \
        union {                                                                \
            typeof(x) __val;                                                   \
            char __c[1];                                                       \
        } __u;                                                                 \
        __read_once_size(&(x), __u.__c, sizeof(x));                            \
        __u.__val;                                                             \
    })

#define smp_cas(__ptr, __old_val, __new_val)                                   \
    __sync_val_compare_and_swap(__ptr, __old_val, __new_val)
#define smp_swap(__ptr, __val) __sync_lock_test_and_set(__ptr, __val)
#define smp_faa(__ptr, __val) __sync_fetch_and_add(__ptr, __val)

#define _Q_LOCKED_VAL 1U
#define _Q_LOCKED_COMBINER_VAL 8U

#define _WAITER_UNPROCESSED 0U
#define _WAITER_PARKED 1U
#define _WAITER_PROCESSING 2U
#define _WAITER_PROCESSED 4U

typedef struct komb_mutex {
    struct komb_node *volatile tail;
    int locked;
    char __pad2[pad_to_cache_line(sizeof(uint32_t))];
#if COND_VAR
    pthread_mutex_t posix_lock;
    char __pad3[pad_to_cache_line(sizeof(pthread_mutex_t))];
#endif
} komb_mutex_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef struct komb_node {
    struct komb_node *volatile next;
    int count;
    int socket_id;
    int cpuid;
    void *rsp;
    komb_mutex_t *lock;
    volatile int wait;
    char dummy1[20];

    union {
        struct {
            uint8_t completed;
            uint8_t locked;
        };
        uint16_t locked_completed;
    };
    char dummy2[48];
} komb_node_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef pthread_cond_t komb_cond_t;
komb_mutex_t *komb_mutex_create(const pthread_mutexattr_t *attr);
int komb_mutex_lock(komb_mutex_t *impl, komb_node_t *me);
int komb_mutex_trylock(komb_mutex_t *impl, komb_node_t *me);
void komb_mutex_unlock(komb_mutex_t *impl, komb_node_t *me);
int komb_mutex_destroy(komb_mutex_t *lock);
int komb_cond_init(komb_cond_t *cond, const pthread_condattr_t *attr);
int komb_cond_timedwait(komb_cond_t *cond, komb_mutex_t *lock, komb_node_t *me,
                        const struct timespec *ts);
int komb_cond_wait(komb_cond_t *cond, komb_mutex_t *lock, komb_node_t *me);
int komb_cond_signal(komb_cond_t *cond);
int komb_cond_broadcast(komb_cond_t *cond);
int komb_cond_destroy(komb_cond_t *cond);
void komb_thread_start(void);
void komb_thread_exit(void);
void komb_application_init(void);
void komb_application_exit(void);
void komb_init_context(komb_mutex_t *impl, komb_node_t *context, int number);

typedef komb_mutex_t lock_mutex_t;
typedef komb_node_t lock_context_t;
typedef komb_cond_t lock_cond_t;

#define lock_mutex_create komb_mutex_create
#define lock_mutex_lock komb_mutex_lock
#define lock_mutex_trylock komb_mutex_trylock
#define lock_mutex_unlock komb_mutex_unlock
#define lock_mutex_destroy komb_mutex_destroy
#define lock_cond_init komb_cond_init
#define lock_cond_timedwait komb_cond_timedwait
#define lock_cond_wait komb_cond_wait
#define lock_cond_signal komb_cond_signal
#define lock_cond_broadcast komb_cond_broadcast
#define lock_cond_destroy komb_cond_destroy
#define lock_thread_start komb_thread_start
#define lock_thread_exit komb_thread_exit
#define lock_application_init komb_application_init
#define lock_application_exit komb_application_exit
#define lock_init_context komb_init_context

#endif // __KOMB_H__
