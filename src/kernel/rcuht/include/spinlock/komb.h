#ifndef __KOMB_H__
#define __KOMB_H__

#ifdef KERNEL_SYNCSTRESS
#include "qspinlock_i.h"
#include "lib/combiner.h"
#define arch_spinlock_t struct orig_qspinlock
#else
#define ENABLE_IRQS_CHECK 1
#include <asm/qspinlock.h>
#endif

#define MAX_NODES 4
#define DEFINE_KOMBSPINLOCK(x)                                                 \
	arch_spinlock_t(x) = (arch_spinlock_t)__ORIG_QSPIN_LOCK_UNLOCKED

//#define KOMB_STATS 1

/*
 * TODO (Correctness optimization): 
 * Add for BIG ENDIAN
 */
struct komb_node {
	struct komb_node *next;
	int tail;
	int count;
	int socket_id;
	int cpuid;
	uint64_t rsp;
	arch_spinlock_t *lock;
	int irqs_disabled;
	struct task_struct *task_struct_ptr;
	char dummy1[12];

	union {
		struct {
			u8 completed;
			u8 locked;
		};
		struct {
			u16 locked_completed;
		};
	};

	char dummy[48];
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
void komb_spin_lock_init(arch_spinlock_t *lock);
void komb_spin_lock(arch_spinlock_t *lock);
void komb_spin_unlock(arch_spinlock_t *lock);
bool komb_spin_trylock(arch_spinlock_t *lock);
void komb_spin_lock_nested(arch_spinlock_t *lock, int level);
void komb_assert_spin_locked(arch_spinlock_t *lock);
int komb_spin_is_locked(arch_spinlock_t *lock);

struct task_struct *komb_get_current(arch_spinlock_t *lock);
void komb_set_current_state(arch_spinlock_t *lock, unsigned int state);
#ifdef KOMB_STATS
void komb_print_stats(void);
#endif

#endif
