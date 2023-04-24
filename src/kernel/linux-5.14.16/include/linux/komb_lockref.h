/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_KOMB_LOCKREF_H
#define __LINUX_KOMB_LOCKREF_H

/*
 * Locked reference counts.
 *
 * These are different from just plain atomic refcounts in that they
 * are atomic with respect to the spinlock that goes with them.  In
 * particular, there can be implementations that don't actually get
 * the spinlock for the common decrement/increment operations, but they
 * still have to check that the operation is done semantically as if
 * the spinlock had been taken (using a cmpxchg operation that covers
 * both the lock and the count word, or using memory transactions, for
 * example).
 */

#include <linux/spinlock.h>
#include <generated/bounds.h>

#define USE_CMPXCHG_LOCKREF                                                    \
	(IS_ENABLED(CONFIG_ARCH_USE_CMPXCHG_LOCKREF) &&                        \
	 IS_ENABLED(CONFIG_SMP) && SPINLOCK_SIZE <= 4)

struct komblockref {
	union {
#if USE_CMPXCHG_LOCKREF
		aligned_u64 lock_count;
#endif
		struct {
			spinlock_t lock;
			int count;
		};
	};
};

extern void komb_lockref_get(struct komblockref *);
extern int komb_lockref_put_return(struct komblockref *);
extern int komb_lockref_get_not_zero(struct komblockref *);
extern int komb_lockref_put_not_zero(struct komblockref *);
extern int komb_lockref_get_or_lock(struct komblockref *);
extern int komb_lockref_put_or_lock(struct komblockref *);

extern void komb_lockref_mark_dead(struct komblockref *);
extern int komb_lockref_get_not_dead(struct komblockref *);

/* Must be called under spinlock for reliable results */
static inline bool __komb_lockref_is_dead(const struct komblockref *l)
{
	return ((int)l->count < 0);
}

#endif /* __LINUX_KOMB_LOCKREF_H */
