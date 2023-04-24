// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/komb_lockref.h>
#include <linux/spinlock.h>

#if USE_CMPXCHG_LOCKREF

/*
 * Note that the "cmpxchg()" reloads the "old" value for the
 * failure case.
 */
#define CMPXCHG_LOOP(CODE, SUCCESS)                                            \
	do {                                                                   \
		int retry = 100;                                               \
		struct komblockref old;                                        \
		BUILD_BUG_ON(sizeof(old) != 8);                                \
		old.lock_count = READ_ONCE(komblockref->lock_count);           \
		while (likely(arch_spin_value_unlocked(old.lock))) {           \
			struct komblockref new = old, prev = old;              \
			CODE old.lock_count =                                  \
				cmpxchg64_relaxed(&komblockref->lock_count,    \
						  old.lock_count,              \
						  new.lock_count);             \
			if (likely(old.lock_count == prev.lock_count)) {       \
				SUCCESS;                                       \
			}                                                      \
			if (!--retry)                                          \
				break;                                         \
			cpu_relax();                                           \
		}                                                              \
	} while (0)

#else

#define CMPXCHG_LOOP(CODE, SUCCESS)                                            \
	do {                                                                   \
	} while (0)

#endif

/**
 * komblockref_get - Increments reference count unconditionally
 * @komblockref: pointer to lockref structure
 *
 * This operation is only valid if you already hold a reference
 * to the object, so you know the count cannot be zero.
 */
void komb_lockref_get(struct komblockref *komblockref)
{
	CMPXCHG_LOOP(new.count++;, return;);

	spin_lock(&komblockref->lock);
	komblockref->count++;
	spin_unlock(&komblockref->lock);
}
EXPORT_SYMBOL_GPL(komb_lockref_get);

/**
 * komblockref_get_not_zero - Increments count unless the count is 0 or dead
 * @komblockref: pointer to lockref structure
 * Return: 1 if count updated successfully or 0 if count was zero
 */
int komb_lockref_get_not_zero(struct komblockref *komblockref)
{
	int retval;

	CMPXCHG_LOOP(new.count++; if (old.count <= 0) return 0;, return 1;);

	spin_lock(&komblockref->lock);
	retval = 0;
	if (komblockref->count > 0) {
		komblockref->count++;
		retval = 1;
	}
	spin_unlock(&komblockref->lock);
	return retval;
}
EXPORT_SYMBOL(komb_lockref_get_not_zero);

/**
 * komblockref_put_not_zero - Decrements count unless count <= 1 before decrement
 * @komblockref: pointer to lockref structure
 * Return: 1 if count updated successfully or 0 if count would become zero
 */
int komb_lockref_put_not_zero(struct komblockref *komblockref)
{
	int retval;

	CMPXCHG_LOOP(new.count--; if (old.count <= 1) return 0;, return 1;);

	spin_lock(&komblockref->lock);
	retval = 0;
	if (komblockref->count > 1) {
		komblockref->count--;
		retval = 1;
	}
	spin_unlock(&komblockref->lock);
	return retval;
}
EXPORT_SYMBOL(komb_lockref_put_not_zero);

/**
 * komblockref_get_or_lock - Increments count unless the count is 0 or dead
 * @komblockref: pointer to lockref structure
 * Return: 1 if count updated successfully or 0 if count was zero
 * and we got the lock instead.
 */
int komb_lockref_get_or_lock(struct komblockref *komblockref)
{
	CMPXCHG_LOOP(new.count++; if (old.count <= 0) break;, return 1;);

	spin_lock(&komblockref->lock);
	if (komblockref->count <= 0)
		return 0;
	komblockref->count++;
	spin_unlock(&komblockref->lock);
	return 1;
}
EXPORT_SYMBOL(komb_lockref_get_or_lock);

/**
 * komblockref_put_return - Decrement reference count if possible
 * @komblockref: pointer to lockref structure
 *
 * Decrement the reference count and return the new value.
 * If the komblockref was dead or locked, return an error.
 */
int komb_lockref_put_return(struct komblockref *komblockref)
{
	CMPXCHG_LOOP(new.count--; if (old.count <= 0) return -1;
		     , return new.count;);
	return -1;
}
EXPORT_SYMBOL(komb_lockref_put_return);

/**
 * komblockref_put_or_lock - decrements count unless count <= 1 before decrement
 * @komblockref: pointer to lockref structure
 * Return: 1 if count updated successfully or 0 if count <= 1 and lock taken
 */
int komb_lockref_put_or_lock(struct komblockref *komblockref)
{
	CMPXCHG_LOOP(new.count--; if (old.count <= 1) break;, return 1;);

	spin_lock(&komblockref->lock);
	if (komblockref->count <= 1)
		return 0;
	komblockref->count--;
	spin_unlock(&komblockref->lock);
	return 1;
}
EXPORT_SYMBOL(komb_lockref_put_or_lock);

/**
 * komblockref_mark_dead - mark lockref dead
 * @komblockref: pointer to lockref structure
 */
void komb_lockref_mark_dead(struct komblockref *komblockref)
{
	assert_spin_locked(&komblockref->lock);
	komblockref->count = -128;
}
EXPORT_SYMBOL(komb_lockref_mark_dead);

/**
 * komblockref_get_not_dead - Increments count unless the ref is dead
 * @komblockref: pointer to lockref structure
 * Return: 1 if count updated successfully or 0 if komblockref was dead
 */
int komb_lockref_get_not_dead(struct komblockref *komblockref)
{
	int retval;

	CMPXCHG_LOOP(new.count++; if (old.count < 0) return 0;, return 1;);

	spin_lock(&komblockref->lock);
	retval = 0;
	if (komblockref->count >= 0) {
		komblockref->count++;
		retval = 1;
	}
	spin_unlock(&komblockref->lock);
	return retval;
}
EXPORT_SYMBOL(komb_lockref_get_not_dead);
