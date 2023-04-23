#ifndef __KOMBRW_H__
#define __KOMBRW_H__

#include "spinlock/qspinlock_i.h"
#include "lib/combiner.h"

#define MAX_NODES 4

#define DEFINE_KOMBSPINLOCK(x)                                                 \
	struct orig_qspinlock(x) =                                             \
		(struct orig_qspinlock)__ORIG_QSPIN_LOCK_UNLOCKED

#define NUMA_AWARE 1

/*
 * Writer states & reader shift and bias.
 */
#define _KOMB_W_WAITING 0x100 /* A writer is waiting	   */
#define _KOMB_W_LOCKED 0x0f0 /* A writer holds the lock */
#define _KOMB_W_LOCKED_SLOW 0x0f /* Acquring write lock in the slow path */
#define _KOMB_W_WMASK 0x1ff /* Writer mask		   */
#define _KOMB_R_SHIFT 9 /* Reader count shift	   */
#define _KOMB_R_BIAS (1U << _KOMB_R_SHIFT)

#define __KOMB_RWLOCK_UNLOCKED                                                 \
	{                                                                      \
		{ .cnts = ATOMIC_INIT(0) }, .lock = __ORIG_QSPIN_LOCK_UNLOCKED \
	}

#define DEFINE_KOMBRWLOCK(x) struct komb_rwlock(x) = __KOMB_RWLOCK_UNLOCKED;

#define _Q_COMPLETED_OFFSET (_Q_LOCKED_OFFSET + _Q_LOCKED_BITS)
#define _Q_COMPLETED_BITS 8
#define _Q_COMPLETED_MASK _Q_SET_MASK(COMPLETED)

#define READ_STATE 0
#define WRITE_STATE 1

//TODO: Add for BIG ENDIAN
struct komb_rwlock_node {
	union {
		struct {
			u8 completed;
			u8 locked;
		};
		struct {
			u16 locked_completed;
		};
	};
	struct komb_rwlock_node *next;
	int tail;
	int count;
	int socket_id;
	int cpuid;
	int state;
	uint64_t rsp;
};

struct komb_rwlock {
	union {
		atomic_t cnts;
		struct {
#ifdef __LITTLE_ENDIAN
			u8 wlocked;
			u8 __lstate[3];
#else
			u8 __lstate[3];
			u8 wlocked;
#endif
		};
	};
	struct orig_qspinlock lock;
};

void komb_rwinit(void); //Called only once on boot. Setup per-core variables
void komb_rwfree(void); //Called only once on exit.

//Called for every spin-lock instance.
void komb_rwlock_init(struct komb_rwlock *lock);
void komb_write_lock(struct komb_rwlock *lock);
void komb_write_unlock(struct komb_rwlock *lock);
void komb_read_lock(struct komb_rwlock *lock);
void komb_read_unlock(struct komb_rwlock *lock);

#endif
