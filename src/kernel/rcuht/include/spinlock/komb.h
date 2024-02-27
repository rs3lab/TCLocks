#ifndef __KOMB_H__
#define __KOMB_H__

#ifdef KERNEL_SYNCSTRESS
#include "qspinlock_i.h"
#include "lib/combiner.h"
#define ENABLE_IRQS_CHECK 0
#define qspinlock orig_qspinlock
#else
#define ENABLE_IRQS_CHECK 1
#include <asm/qspinlock.h>
#endif

#define MAX_NODES 4
#define DEFINE_KOMBSPINLOCK(x)                                                 \
	struct qspinlock(x) = (struct qspinlock)__ORIG_QSPIN_LOCK_UNLOCKED

/*
 * TODO (Correctness optimization): 
 * Add for BIG ENDIAN
 */
struct komb_node {
	union{
		struct {
			struct komb_node *next;
			int tail;
			int count;
			int socket_id;
			int cpuid;
			void* rsp;
			struct qspinlock *lock;
			int irqs_disabled;
			struct task_struct *task_struct_ptr;
		};
		char alignment[128];
	};

	union {
		struct {
			u8 completed;
			u8 locked;
		};
		struct {
			u16 locked_completed;
		};
		char alignment1[128];
	};
};

#define _Q_COMPLETED_OFFSET (_Q_LOCKED_OFFSET + _Q_LOCKED_BITS)
#define _Q_COMPLETED_BITS 8
#define _Q_COMPLETED_MASK _Q_SET_MASK(COMPLETED)

/*
 * komb_init and komb_free should be called only when the system boots up and
 * shut down. They are used to setup and free per-core variables.
 */
void komb_init(void);
void komb_free(void);

/*
 * Public API
 */
extern void komb_spin_lock_init(struct qspinlock *lock);
extern int komb_spin_is_locked(struct qspinlock *lock);
extern int komb_spin_value_unlocked(struct qspinlock lock);
extern int komb_spin_is_contended(struct qspinlock *lock);
extern int komb_spin_trylock(struct qspinlock *lock);
extern void komb_spin_lock(struct qspinlock *lock);
extern void komb_spin_unlock(struct qspinlock *lock);

struct task_struct *komb_get_current(spinlock_t *lock);
void komb_set_current_state(spinlock_t *lock, unsigned int state);

#ifdef KOMB_STATS
void komb_print_stats(void);
#endif

#endif
