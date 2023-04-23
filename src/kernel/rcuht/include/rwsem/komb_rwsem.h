#ifndef __LINUX_KOMB_RWSEM_H
#define __LINUX_KOMB_RWSEM_H

#include <asm/current.h>
#include <linux/spinlock_types.h>
#include <linux/atomic.h>
#include <asm/processor.h>

#ifdef KERNEL_SYNCSTRESS
#include "spinlock/aqs.h"
#include "mutex/komb_mutex.h"
#else
#include <linux/komb_mutex.h>
#endif

#define WRITE_BOUNDED_OPPORTUNISTIC_SPIN 0

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

#define NUM_SLOT	(1024)
#define TABLE_SIZE	((NUM_SLOT)*8)
#define V(i)		((i)*8)

#define CHECK_FOR_BIAS		16
#define MULTIPLIER 9

/*struct komb_mutex_node {
	struct komb_mutex_node *next;
	struct komb_mutex_node *tail;

	int socket_id; //Socket ID
	int cpuid;

	uint64_t rsp;
	struct task_struct *task_struct_ptr;
	void *lock;

	char dummy1[80];

	union {
		atomic_long_t val;
		atomic_long_t cnts;
		struct {
			u8 completed;
			u8 locked;
			u8 __unused[6];
		};
		struct {
			u16 locked_completed;
			u8 __unused1[6];
		};
		struct {
			u16 wlocked;
			u8 rcount[6];
		};
	};

	char dummy[16];
};*/

struct komb_rwsem {
	union {
		atomic_long_t cnts;
		struct {
			u8 wlocked;
			u8 rcount[7];
		};
	};
	char dummy1[128];
	char dummy2[128];
	struct aqs_lock reader_wait_lock; 
	char dummy3[128];
	struct komb_mutex_node *writer_tail;
	char dummy4[128];
	struct komb_mutex_node *komb_waiter_rsp_ptr;
	struct komb_mutex_node *komb_waiter_node;
#ifdef BRAVO
	int rbias;
	u64 inhibit_until;
#endif
};

#define __KOMB_RWSEM_EXTRA_INIT(name)  				\
	{.cnts = ATOMIC_LONG_INIT(0), .writer_tail = NULL}


#define __KOMB_RWSEM_INITIALIZER(lockname)                                     \
{.cnts = ATOMIC_LONG_INIT(0), .writer_tail = NULL, .reader_wait_lock.val = ATOMIC_INIT(0)}

#define DECLARE_KOMB_RWSEM(krwsemname)                                         \
	struct komb_rwsem krwsemname = __KOMB_RWSEM_INITIALIZER(krwsemname)

static inline int komb_rwsem_is_locked(struct komb_rwsem *sem)
{
	return atomic_long_read(&sem->cnts) != 0;
}

#define init_komb_rwsem(sem)                                                   \
	do {                                                                   \
		static struct lock_class_key __key;                            \
		__init_komb_rwsem((sem), #sem, &__key);                        \
	} while (0)

extern void komb_rwsem_init(void);

static inline int komb_rwsem_is_contended(struct komb_rwsem *sem)
{
	return (sem->writer_tail != NULL);
}

extern void __init_komb_rwsem(struct komb_rwsem *sem, const char *name,
			      struct lock_class_key *key);

extern void komb_rwsem_down_read(struct komb_rwsem *sem);
extern int __must_check komb_rwsem_down_read_killable(struct komb_rwsem *sem);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
extern int komb_rwsem_down_read_trylock(struct komb_rwsem *sem);

/*
 * lock for writing
 */
extern void komb_rwsem_down_write(struct komb_rwsem *sem);
extern int __must_check komb_rwsem_down_write_killable(struct komb_rwsem *sem);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
extern int komb_rwsem_down_write_trylock(struct komb_rwsem *sem);

/*
 * release a read lock
 */
extern void komb_rwsem_up_read(struct komb_rwsem *sem);

/*
 * release a write lock
 */
extern void komb_rwsem_up_write(struct komb_rwsem *sem);

/*
 * downgrade write lock to read lock
 */
extern void komb_rwsem_downgrade_write(struct komb_rwsem *sem);

extern void komb_rwsem_down_read_nested(struct komb_rwsem *sem, int level);
extern void komb_rwsem_down_write_nested(struct komb_rwsem *sem, int level);

extern int komb_rwsem_down_write_killable_nested(struct komb_rwsem *sem,
						 int level);

#endif /* __LINUX_KOMB_RWSEM_H */
