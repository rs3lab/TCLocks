/* SPDX-License-Identifier: GPL-2.0 */
/* rwsem.h: R/W semaphores, public interface
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from asm-i386/semaphore.h
 */

#ifndef _LINUX_RWSEM_H
#define _LINUX_RWSEM_H

#include <linux/linkage.h>

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/err.h>

struct rw_semaphore;

struct rwaqm_node {
	struct rwaqm_node *next;

	union {
		unsigned int locked;
		struct {
			u8  lstatus;
			u8  sleader;
			u16 wcount;
		};
	};

	int nid;
        struct task_struct *task;
	struct rwaqm_node *last_visited;
	int type;
	int lock_status;
} ____cacheline_aligned;

struct rwmutex {
	struct rwaqm_node *tail;
	union {
		atomic_t val;
#ifdef __LITTLE_ENDIAN
		struct {
			u8 locked;
			u8 no_stealing;
			u8 rtype_cur;
			u8 rtype_new;
		};
		struct {
			u8 locked_no_stealing;
		};
#else
		struct {
			u8  __unused[2];
			u8  no_stealing;
			u8  locked;
		};
		struct {
			u16 __unused2;
			u16 locked_no_stealing;
		};
#endif
	};
};

struct rw_semaphore {
	union {
		atomic_long_t cnts;
		struct {
			u8 wlocked;
			u8 rcount[7];
		};
	};
	struct rwmutex wait_lock;
#ifdef USE_GLOBAL_RDTABLE
	uint64_t *skt_readers;
	uint64_t *cpu_readers;
#endif
};

#define RWAQM_UNLOCKED_VALUE 	0x00000000L
#define	RWAQM_W_WAITING	        0x100		/* A writer is waiting	   */
#define	RWAQM_W_LOCKED	        0x0bf		/* A writer holds the lock */
#define	RWAQM_W_WMASK	        0x1bf		/* Writer mask		   */
#define	RWAQM_R_SHIFT	        9		/* Reader count shift	   */
#define RWAQM_R_BIAS	        (1U << RWAQM_R_SHIFT)

#define RWAQM_R_CNTR_CTR 0x1         /* Reader is centralized */
#define RWAQM_R_NUMA_CTR 0x2 		/* Reader is per-socket */
#define RWAQM_R_PCPU_CTR 0x4 		/* Reader is per-core */
#define RWAQM_R_WRON_CTR 0x8 		/* All readers behave as writers */

#define RWAQM_DCTR(v)        (((v) << 8) | (v))
#define RWAQM_R_CNTR_DCTR    RWAQM_DCTR(RWAQM_R_CNTR_CTR)
#define RWAQM_R_NUMA_DCTR    RWAQM_DCTR(RWAQM_R_NUMA_CTR)
#define RWAQM_R_PCPU_DCTR    RWAQM_DCTR(RWAQM_R_PCPU_CTR)
#define RWAQM_R_WRON_DCTR    RWAQM_DCTR(RWAQM_R_WRON_CTR)


#define __RWMUTEX_INITIALIZER(lockname) 			\
	{ .val = ATOMIC_INIT(0) 				\
	, .tail = NULL }

#define DEFINE_RWMUTEX(rwmutexname) \
	struct rwmutex rwmutexname = __RWMUTEX_INITIALIZER(rwmutexname)


#ifdef USE_GLOBAL_RDTABLE
#define __INIT_TABLE(name) , .skt_readers = NULL, .cpu_readers = NULL
#else
#define __INIT_TABLE(name)
#endif

#ifdef SEPARATE_PARKING_LIST
#define __INIT_SEPARATE_PLIST(name) 				\
	, .wait_slock = __RAW_SPIN_LOCK_UNLOCKED(name.wait_slock) \
	, .next = NULL
#else
#define __INIT_SEPARATE_PLIST(name)
#endif

#define __RWAQM_INIT_COUNT(name)  				\
	.cnts = ATOMIC_LONG_INIT(RWAQM_UNLOCKED_VALUE)


/* Include the arch specific part */
/* #include <asm/rwsem.h> */

/* In all implementations count != 0 means locked */
static inline int rwsem_is_locked(struct rw_semaphore *sem)
{
	return atomic_long_read(&sem->cnts) != 0;
}

#define RWSEM_UNLOCKED_VALUE		0L
#define __RWSEM_COUNT_INIT(name)	.count = ATOMIC_LONG_INIT(RWSEM_UNLOCKED_VALUE)

/* Common initializer macros and functions */

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define __RWSEM_DEP_MAP_INIT(lockname)			\
	.dep_map = {					\
		.name = #lockname,			\
		.wait_type_inner = LD_WAIT_SLEEP,	\
	},
#else
# define __RWSEM_DEP_MAP_INIT(lockname)
#endif

#ifdef CONFIG_DEBUG_RWSEMS
# define __RWSEM_DEBUG_INIT(lockname) .magic = &lockname,
#else
# define __RWSEM_DEBUG_INIT(lockname)
#endif

#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
#define __RWSEM_OPT_INIT(lockname) , .osq = OSQ_LOCK_UNLOCKED, .owner = NULL
#else
#define __RWSEM_OPT_INIT(lockname)
#endif

#define __RWSEM_INITIALIZER(name)				\
	{ __RWAQM_INIT_COUNT(name)  				\
	, __RWMUTEX_INITIALIZER((name).wait_lock) 		\
	  __INIT_TABLE((name)) 					\
	  __INIT_SEPARATE_PLIST((name)) }

#define DECLARE_RWSEM(name) \
	struct rw_semaphore name = __RWSEM_INITIALIZER(name)

extern void __init_rwsem(struct rw_semaphore *sem, const char *name,
			 struct lock_class_key *key);

#define init_rwsem(sem)						\
do {								\
	static struct lock_class_key __key;			\
								\
	__init_rwsem((sem), #sem, &__key);			\
} while (0)

/*
 * This is the same regardless of which rwsem implementation that is being used.
 * It is just a heuristic meant to be called by somebody already holding the
 * rwsem to see if somebody from an incompatible type is wanting access to the
 * lock.
 */
static inline int rwsem_is_contended(struct rw_semaphore *sem)
{
	return sem->wait_lock.tail != NULL;
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


# define down_read_nested(sem, subclass) down_read(sem)
# define down_read_killable_nested(sem, subclass)	down_read_killable(sem)
# define down_write_nest_lock(sem, nest_lock)	down_write(sem)
# define down_write_nested(sem, subclass)	down_write(sem)
# define down_write_killable_nested(sem, subclass)	down_write_killable(sem)
# define down_read_non_owner(sem)		down_read(sem)
# define up_read_non_owner(sem)			up_read(sem)

#endif /* _LINUX_RWSEM_H */
