#ifndef _AQS_H_
#define _AQS_H_

#include <linux/typecheck.h>
#include <linux/preempt.h>
#include <linux/linkage.h>
#include <linux/compiler.h>
#include <linux/irqflags.h>
#include <linux/thread_info.h>
#include <linux/kernel.h>
#include <linux/stringify.h>
#include <linux/bottom_half.h>
#include <linux/lockdep.h>
#include <asm/barrier.h>
#include <asm/mmiowb.h>


/*
 * Currently, we only support up to 16K CPUs
 *
 *  0- 7: locked byte
 *     1: disabled stealing
 *  9-15: numa id (+1) for NUMA-aware lock stealing
 * 16-17: tail index (task context)
 * 18-31: tail cpu (+1)
 *
 * OR
 *
 *  0- 7: locked byte
 *  8-15: no stealing up to 128 queue length(unfair) or until the tail (fair)
 * 16-17: tail index (task context)
 * 18-31: tail cpu (+1)
 */
#define	_AQS_SET_MASK(type)	(((1U << _AQS_ ## type ## _BITS) - 1)\
                                 << _AQS_ ## type ## _OFFSET)

/* This is directly used by the tail bytes (2 bytes) */
#define _AQS_TAIL_IDX_OFFSET	(0)
#define _AQS_TAIL_IDX_BITS	2
#define _AQS_TAIL_IDX_MASK	_AQS_SET_MASK(TAIL_IDX)

#define _AQS_TAIL_CPU_OFFSET	(_AQS_TAIL_IDX_OFFSET + _AQS_TAIL_IDX_BITS)
#define _AQS_TAIL_CPU_BITS	(16 - _AQS_TAIL_CPU_OFFSET)
#define _AQS_TAIL_CPU_MASK	_AQS_SET_MASK(TAIL_CPU)

#define _AQS_TAIL_OFFSET	_AQS_TAIL_IDX_OFFSET
#define _AQS_TAIL_MASK		(_AQS_TAIL_IDX_MASK | _AQS_TAIL_CPU_MASK)

/* Use 1 bit for the NOSTEAL part */
#define _AQS_NOSTEAL_OFFSET     0
#define _AQS_NOSTEAL_BITS       1
#define _AQS_NOSTEAL_MASK       _AQS_SET_MASK(NOSTEAL)

/* We can support up to 127 sockets for NUMA-aware fastpath stealing */
#define _AQS_NUMA_ID_OFFSET     (_AQS_NOSTEAL_OFFSET + _AQS_NOSTEAL_BITS)
#define _AQS_NUMA_ID_BITS       7
#define _AQS_NUMA_ID_MASK       _AQS_SET_MASK(NUMA_ID)
#define _AQS_NUMA_ID_VAL(v)     ((v) & _AQS_NUMA_ID_MASK) >> _AQS_NUMA_ID_OFFSET

#define _AQS_LOCKED_OFFSET              0
#define _AQS_LOCKED_BITS                8
#define _AQS_LOCKED_NOSTEAL_OFFSET      (_AQS_LOCKED_OFFSET + _AQS_LOCKED_BITS)

#define AQS_NOSTEAL_VAL         1
#define AQS_STATUS_WAIT         0
#define AQS_STATUS_LOCKED       1
#define AQS_MAX_LOCK_COUNT      256
#define AQS_SERVE_COUNT         (255) /* max of 8 bits */

struct aqs_node {
        struct aqs_node *next;
        union {
                u32 locked;
                struct {
                        u8 lstatus;
                        u8 sleader;
                        u16 wcount;
                };
        };

        int nid;
        int cid;
        int count;
        int rv;

        struct aqs_node *last_visited;
};

struct aqs_lock {
        union {
                atomic_t val;
#ifdef __LITTLE_ENDIAN
                struct {
                        u8	locked; /* Must be 1, if entering a CS */
                        u8	no_stealing; /* Allows fastpath stealing */
                };
                struct {
                        u16     locked_no_stealing; /* Represents the above */
                        u16     tail; /* Encode cpu and task context */
                };
#else
                struct {
                        u8      reserved[2];
                        u8	no_stealing;
                        u8	locked;
                };
                struct {
                        u16     tail;
                        u16     locked_no_stealing;
                };
#endif
        };
};

#define __AQS_LOCK_UNLOCKED { .val = ATOMIC_INIT(0) }

#define DEFINE_AQSLOCK(x) \
        struct aqs_lock x = __AQS_LOCK_UNLOCKED;

#define assert_aqs_spin_locked(lock) \
        BUG_ON(!aqs_spin_is_locked(lock))

static __always_inline
int aqs_spin_value_unlocked(struct aqs_lock lock)
{
        return !READ_ONCE(lock.locked);
}

static __always_inline
int aqs_spin_is_locked(struct aqs_lock *lock)
{
	/* return READ_ONCE(lock->locked); */
        return atomic_read(&lock->val);
}

static inline void
aqs_spin_lock_init(struct aqs_lock *lock)
{
        atomic_set(&lock->val, 0);
}

/* ----------- Acquire APIs --------------- */
extern void aqs_spin_lock_slowpath(struct aqs_lock *lock);

static __always_inline
void __aqs_acquire(struct aqs_lock *lock)
{
        int ret;

        ret = cmpxchg(&lock->locked_no_stealing, 0, 1);
        if (likely(ret == 0))
                return;
        aqs_spin_lock_slowpath(lock);
}

static __always_inline
void aqs_spin_lock(struct aqs_lock *lock)
{
        preempt_disable();
        __aqs_acquire(lock);

}

/* ----------- Release APIs --------------- */
static __always_inline
void __aqs_release(struct aqs_lock *lock)
{
        smp_store_release(&lock->locked, 0);
}

static __always_inline
void aqs_spin_unlock(struct aqs_lock *lock)
{
        __aqs_release(lock);
        preempt_enable();
}

/* ----------- trylock APIs --------------- */
static __always_inline
int aqs_spin_trylock(struct aqs_lock *lock)
{
        preempt_disable();
        if (!READ_ONCE(lock->locked_no_stealing) &&
            cmpxchg(&lock->locked_no_stealing, 0, 1) == 0)
                return 1;

        preempt_enable();
        return 0;
}

#endif /* _AQS_H_ */
