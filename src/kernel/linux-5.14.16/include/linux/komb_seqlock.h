/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_KOMB_SEQLOCK_H
#define __LINUX_KOMB_SEQLOCK_H

/*
 * seqcount_t / komb_seqlock_t - a reader-writer consistency mechanism with
 * lockless readers (read-only retry loops), and no writer starvation.
 *
 * See Documentation/locking/seqlock.rst
 *
 * Copyrights:
 * - Based on x86_64 vsyscall gettimeofday: Keith Owens, Andrea Arcangeli
 * - Sequence counters with associated locks, (C) 2020 Linutronix GmbH
 */

#include <linux/compiler.h>
#include <linux/kcsan-checks.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/ww_mutex.h>
#include <linux/preempt.h>
#include <linux/spinlock.h>

#include <asm/processor.h>
#include <asm/qspinlock.h>
/*
 * Sequential locks (komb_seqlock_t)
 *
 * Sequence counters with an embedded spinlock for writer serialization
 * and non-preemptibility.
 *
 * For more info, see:
 *    - Comments on top of seqcount_t
 *    - Documentation/locking/seqlock.rst
 */
typedef struct {
	/*
	 * Make sure that readers don't starve writers on PREEMPT_RT: use
	 * seqcount_spinlock_t instead of seqcount_t. Check __SEQ_LOCK().
	 */
	seqcount_spinlock_t seqcount;
	spinlock_t lock;
} komb_seqlock_t;

#define __KOMB_SEQLOCK_UNLOCKED(lockname)                                      \
	{                                                                      \
		.seqcount = SEQCNT_SPINLOCK_ZERO(lockname, &(lockname).lock),  \
		.lock = ATOMIC_INIT(0)                                         \
	}

/**
 * seqlock_init() - dynamic initializer for komb_seqlock_t
 * @sl: Pointer to the komb_seqlock_t instance
 */
#define komb_seqlock_init(sl)                                                  \
	do {                                                                   \
		spin_lock_init(&(sl)->lock);                              \
		seqcount_spinlock_init(&(sl)->seqcount, &(sl)->lock);          \
	} while (0)

/**
 * DEFINE_SEQLOCK(sl) - Define a statically allocated komb_seqlock_t
 * @sl: Name of the komb_seqlock_t instance
 */
#define DEFINE_KOMB_SEQLOCK(sl) komb_seqlock_t sl = __KOMB_SEQLOCK_UNLOCKED(sl)

/**
 * read_seqbegin() - start a komb_seqlock_t read side critical section
 * @sl: Pointer to komb_seqlock_t
 *
 * Return: count, to be passed to read_seqretry()
 */
static inline unsigned komb_read_seqbegin(const komb_seqlock_t *sl)
{
	unsigned ret = read_seqcount_begin(&sl->seqcount);

	kcsan_atomic_next(
		0); /* non-raw usage, assume closing read_seqretry() */
	kcsan_flat_atomic_begin();
	return ret;
}

/**
 * read_seqretry() - end a komb_seqlock_t read side section
 * @sl: Pointer to komb_seqlock_t
 * @start: count, from read_seqbegin()
 *
 * read_seqretry closes the read side critical section of given komb_seqlock_t.
 * If the critical section was invalid, it must be ignored (and typically
 * retried).
 *
 * Return: true if a read section retry is required, else false
 */
static inline unsigned komb_read_seqretry(const komb_seqlock_t *sl,
					  unsigned start)
{
	/*
	 * Assume not nested: read_seqretry() may be called multiple times when
	 * completing read critical section.
	 */
	kcsan_flat_atomic_end();

	return read_seqcount_retry(&sl->seqcount, start);
}

/*
 * For all komb_seqlock_t write side functions, use the the internal
 * do_write_seqcount_begin() instead of generic write_seqcount_begin().
 * This way, no redundant lockdep_assert_held() checks are added.
 */

/**
 * write_seqlock() - start a komb_seqlock_t write side critical section
 * @sl: Pointer to komb_seqlock_t
 *
 * write_seqlock opens a write side critical section for the given
 * komb_seqlock_t.  It also implicitly acquires the spinlock_t embedded inside
 * that sequential lock. All komb_seqlock_t write side sections are thus
 * automatically serialized and non-preemptible.
 *
 * Context: if the komb_seqlock_t read section, or other write side critical
 * sections, can be invoked from hardirq or softirq contexts, use the
 * _irqsave or _bh variants of this function instead.
 */
static inline void komb_write_seqlock(komb_seqlock_t *sl)
{
	spin_lock(&sl->lock);
	do_write_seqcount_begin(&sl->seqcount.seqcount);
}

/**
 * write_sequnlock() - end a komb_seqlock_t write side critical section
 * @sl: Pointer to komb_seqlock_t
 *
 * write_sequnlock closes the (serialized and non-preemptible) write side
 * critical section of given komb_seqlock_t.
 */
static inline void komb_write_sequnlock(komb_seqlock_t *sl)
{
	do_write_seqcount_end(&sl->seqcount.seqcount);
	spin_unlock(&sl->lock);
}

/**
 * read_seqlock_excl() - begin a komb_seqlock_t locking reader section
 * @sl:	Pointer to komb_seqlock_t
 *
 * read_seqlock_excl opens a komb_seqlock_t locking reader critical section.  A
 * locking reader exclusively locks out *both* other writers *and* other
 * locking readers, but it does not update the embedded sequence number.
 *
 * Locking readers act like a normal spin_lock()/spin_unlock().
 *
 * Context: if the komb_seqlock_t write section, *or other read sections*, can
 * be invoked from hardirq or softirq contexts, use the _irqsave or _bh
 * variant of this function instead.
 *
 * The opened read section must be closed with read_sequnlock_excl().
 */
static inline void komb_read_seqlock_excl(komb_seqlock_t *sl)
{
	spin_lock(&sl->lock);
}

/**
 * read_sequnlock_excl() - end a komb_seqlock_t locking reader critical section
 * @sl: Pointer to komb_seqlock_t
 */
static inline void komb_read_sequnlock_excl(komb_seqlock_t *sl)
{
	spin_unlock(&sl->lock);
}

/**
 * read_seqbegin_or_lock() - begin a komb_seqlock_t lockless or locking reader
 * @lock: Pointer to komb_seqlock_t
 * @seq : Marker and return parameter. If the passed value is even, the
 * reader will become a *lockless* komb_seqlock_t reader as in read_seqbegin().
 * If the passed value is odd, the reader will become a *locking* reader
 * as in read_seqlock_excl().  In the first call to this function, the
 * caller *must* initialize and pass an even value to @seq; this way, a
 * lockless read can be optimistically tried first.
 *
 * read_seqbegin_or_lock is an API designed to optimistically try a normal
 * lockless komb_seqlock_t read section first.  If an odd counter is found, the
 * lockless read trial has failed, and the next read iteration transforms
 * itself into a full komb_seqlock_t locking reader.
 *
 * This is typically used to avoid komb_seqlock_t lockless readers starvation
 * (too much retry loops) in the case of a sharp spike in write side
 * activity.
 *
 * Context: if the komb_seqlock_t write section, *or other read sections*, can
 * be invoked from hardirq or softirq contexts, use the _irqsave or _bh
 * variant of this function instead.
 *
 * Check Documentation/locking/seqlock.rst for template example code.
 *
 * Return: the encountered sequence counter value, through the @seq
 * parameter, which is overloaded as a return parameter. This returned
 * value must be checked with need_seqretry(). If the read section need to
 * be retried, this returned value must also be passed as the @seq
 * parameter of the next read_seqbegin_or_lock() iteration.
 */
static inline void komb_read_seqbegin_or_lock(komb_seqlock_t *lock, int *seq)
{
	if (!(*seq & 1)) /* Even */
		*seq = komb_read_seqbegin(lock);
	else /* Odd */
		komb_read_seqlock_excl(lock);
}

/**
 * need_seqretry() - validate komb_seqlock_t "locking or lockless" read section
 * @lock: Pointer to komb_seqlock_t
 * @seq: sequence count, from read_seqbegin_or_lock()
 *
 * Return: true if a read section retry is required, false otherwise
 */
static inline int komb_need_seqretry(komb_seqlock_t *lock, int seq)
{
	return !(seq & 1) && komb_read_seqretry(lock, seq);
}

/**
 * done_seqretry() - end komb_seqlock_t "locking or lockless" reader section
 * @lock: Pointer to komb_seqlock_t
 * @seq: count, from read_seqbegin_or_lock()
 *
 * done_seqretry finishes the komb_seqlock_t read side critical section started
 * with read_seqbegin_or_lock() and validated by need_seqretry().
 */
static inline void komb_done_seqretry(komb_seqlock_t *lock, int seq)
{
	if (seq & 1)
		komb_read_sequnlock_excl(lock);
}

#endif /* __LINUX_KOMB_SEQLOCK_H */
