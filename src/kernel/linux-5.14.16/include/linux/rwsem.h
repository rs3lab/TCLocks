/* SPDX-License-Identifier: GPL-2.0 */
/* rwsem.h: R/W semaphores, public interface
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from asm-i386/semaphore.h
 */

#ifndef _LINUX_RWSEM_H
#define _LINUX_RWSEM_H

#include <linux/linkage.h>
#include <linux/aqm.h>

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/mutex.h>

//#define BRAVO 1

/*
 * Writer states & reader shift and bias.
 */
#define _KOMB_RWSEM_W_LOCKED 0xff /* A writer holds the lock */
#define _KOMB_RWSEM_W_COMBINER 0x70 /* A combiner holds the lock */
#define _KOMB_RWSEM_W_OOO 0x7f /* A combiner holds the lock */
#define _KOMB_RWSEM_W_WMASK 0x1ff /* Writer mask		   */
#define _KOMB_RWSEM_W_WAITING 0x100 /* Writer waiting */
#define _KOMB_RWSEM_R_SHIFT 9 /* Reader count shift	   */
#define _KOMB_RWSEM_R_BIAS (1U << _KOMB_RWSEM_R_SHIFT)

#define _Q_COMPLETED_OFFSET (_Q_LOCKED_OFFSET + _Q_LOCKED_BITS)
#define _Q_COMPLETED_BITS 8
#define _Q_COMPLETED_MASK _Q_SET_MASK(COMPLETED)

#define NUM_SLOT (1024)
#define TABLE_SIZE ((NUM_SLOT))
#define V(i) ((i))

#define CHECK_FOR_BIAS 16
#define MULTIPLIER 9

/*
 * For an uncontended rwsem, count and owner are the only fields a task
 * needs to touch when acquiring the rwsem. So they are put next to each
 * other to increase the chance that they will share the same cacheline.
 *
 * In a contended rwsem, the owner is likely the most frequently accessed
 * field in the structure as the optimistic waiter that holds the osq lock
 * will spin on owner. For an embedded rwsem, other hot fields in the
 * containing structure should be moved further away from the rwsem to
 * reduce the chance that they will share the same cacheline causing
 * cacheline bouncing problem.
 */
struct rw_semaphore {
	union {
		atomic_long_t cnts;
		struct {
			u8 wlocked;
			u8 rcount[7];
		};
	};
	char dummy1[128];
	char dummy2[128];
	struct aqm_mutex reader_wait_lock;
	char dummy3[128];
	struct mutex_node *writer_tail;
	char dummy4[128];
	struct mutex_node *komb_waiter_rsp_ptr;
	struct mutex_node *komb_waiter_node;
#ifdef BRAVO
	int rbias;
	u64 inhibit_until;
#endif
};

/* In all implementations count != 0 means locked */
static inline int rwsem_is_locked(struct rw_semaphore *sem)
{
	return atomic_long_read(&sem->cnts) != 0;
}

#define RWSEM_UNLOCKED_VALUE 0L

/* Common initializer macros and functions */

#define __RWSEM_DEP_MAP_INIT(lockname)

#define __RWSEM_DEBUG_INIT(lockname)

#define __RWSEM_OPT_INIT(lockname)

#define __RWSEM_INITIALIZER(lockname)                                          \
	{                                                                      \
		.cnts = ATOMIC_LONG_INIT(0),                                   \
		.reader_wait_lock.val = ATOMIC_INIT(0),                        \
		.reader_wait_lock.tail = NULL, .writer_tail = NULL             \
	}

#define DECLARE_RWSEM(name) struct rw_semaphore name = __RWSEM_INITIALIZER(name)

extern void __init_rwsem(struct rw_semaphore *sem, const char *name,
			 struct lock_class_key *key);

#define init_rwsem(sem)                                                        \
	do {                                                                   \
		static struct lock_class_key __key;                            \
                                                                               \
		__init_rwsem((sem), #sem, &__key);                             \
	} while (0)

extern void komb_rwsem_init(void);

/*
 * This is the same regardless of which rwsem implementation that is being used.
 * It is just a heuristic meant to be called by somebody already holding the
 * rwsem to see if somebody from an incompatible type is wanting access to the
 * lock.
 */
static inline int rwsem_is_contended(struct rw_semaphore *sem)
{
	return (sem->writer_tail != NULL);
}

/*
 * lock for reading
 */
extern void down_read(struct rw_semaphore *sem);
extern int __must_check down_read_interruptible(struct rw_semaphore *sem);
extern int __must_check down_read_killable(struct rw_semaphore *sem);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
extern int down_read_trylock(struct rw_semaphore *sem);

/*
 * lock for writing
 */
extern void down_write(struct rw_semaphore *sem);
extern int __must_check down_write_killable(struct rw_semaphore *sem);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
extern int down_write_trylock(struct rw_semaphore *sem);

/*
 * release a read lock
 */
extern void up_read(struct rw_semaphore *sem);

/*
 * release a write lock
 */
extern void up_write(struct rw_semaphore *sem);

/*
 * downgrade write lock to read lock
 */
extern void downgrade_write(struct rw_semaphore *sem);

#define down_read_nested(sem, subclass) down_read(sem)
#define down_read_killable_nested(sem, subclass) down_read_killable(sem)
#define down_write_nest_lock(sem, nest_lock) down_write(sem)
#define down_write_nested(sem, subclass) down_write(sem)
#define down_write_killable_nested(sem, subclass) down_write_killable(sem)
#define down_read_non_owner(sem) down_read(sem)
#define up_read_non_owner(sem) up_read(sem)

#endif /* _LINUX_RWSEM_H */
