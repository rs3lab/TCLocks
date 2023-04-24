#ifndef __UCOMB_H__
#define __UCOMB_H__

#include "padding.h"
#define LOCK_ALGORITHM "UCOMB"
#define NEED_CONTEXT 1
#define SUPPORT_WAITING 1

struct ContextSwitchFrame {
	uint64_t rsp;
	uint64_t rbp;
	uint64_t rbx;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
} __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef struct ucomb_node {
    struct ContextSwitchFrame frame_;
    struct ucomb_node *_Atomic next;
    volatile int locked;
    volatile int completed;
    volatile int next_owner;
    int numa_node_id;
} ucomb_node_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef struct ucomb_mutex {
#if COND_VAR
    pthread_mutex_t posix_lock;
    char __pad[pad_to_cache_line(sizeof(pthread_mutex_t))];
#endif
    /* struct ucomb_node *volatile tail __attribute__((aligned(L_CACHE_LINE_SIZE))); */
    struct ucomb_node *_Atomic tail;
} ucomb_mutex_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef pthread_cond_t ucomb_cond_t;
ucomb_mutex_t *ucomb_mutex_create(const pthread_mutexattr_t *attr);
int ucomb_mutex_lock(ucomb_mutex_t *impl, ucomb_node_t *me);
int ucomb_mutex_trylock(ucomb_mutex_t *impl, ucomb_node_t *me);
void ucomb_mutex_unlock(ucomb_mutex_t *impl, ucomb_node_t *me);
int ucomb_mutex_destroy(ucomb_mutex_t *lock);
int ucomb_cond_init(ucomb_cond_t *cond, const pthread_condattr_t *attr);
int ucomb_cond_timedwait(ucomb_cond_t *cond, ucomb_mutex_t *lock, ucomb_node_t *me,
                       const struct timespec *ts);
int ucomb_cond_wait(ucomb_cond_t *cond, ucomb_mutex_t *lock, ucomb_node_t *me);
int ucomb_cond_signal(ucomb_cond_t *cond);
int ucomb_cond_broadcast(ucomb_cond_t *cond);
int ucomb_cond_destroy(ucomb_cond_t *cond);
void ucomb_thread_start(void);
void ucomb_thread_exit(void);
void ucomb_application_init(void);
void ucomb_application_exit(void);
void ucomb_init_context(ucomb_mutex_t *impl, ucomb_node_t *context, int number);

typedef ucomb_mutex_t lock_mutex_t;
typedef ucomb_node_t lock_context_t;
typedef ucomb_cond_t lock_cond_t;

#define lock_mutex_create ucomb_mutex_create
#define lock_mutex_lock ucomb_mutex_lock
#define lock_mutex_trylock ucomb_mutex_trylock
#define lock_mutex_unlock ucomb_mutex_unlock
#define lock_mutex_destroy ucomb_mutex_destroy
#define lock_cond_init ucomb_cond_init
#define lock_cond_timedwait ucomb_cond_timedwait
#define lock_cond_wait ucomb_cond_wait
#define lock_cond_signal ucomb_cond_signal
#define lock_cond_broadcast ucomb_cond_broadcast
#define lock_cond_destroy ucomb_cond_destroy
#define lock_thread_start ucomb_thread_start
#define lock_thread_exit ucomb_thread_exit
#define lock_application_init ucomb_application_init
#define lock_application_exit ucomb_application_exit
#define lock_init_context ucomb_init_context

#endif // __UCOMB_H__
