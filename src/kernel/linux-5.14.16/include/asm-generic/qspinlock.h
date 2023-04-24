/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Queued spinlock
 *
 * (C) Copyright 2013-2015 Hewlett-Packard Development Company, L.P.
 * (C) Copyright 2015 Hewlett-Packard Enterprise Development LP
 *
 * Authors: Waiman Long <waiman.long@hpe.com>
 */
#ifndef __ASM_GENERIC_QSPINLOCK_H
#define __ASM_GENERIC_QSPINLOCK_H

#include <asm-generic/qspinlock_types.h>
#include <linux/atomic.h>
#include <linux/komb.h>

#ifndef virt_spin_lock
static __always_inline bool virt_spin_lock(struct qspinlock *lock)
{
	return false;
}
#endif

/*
 * Remapping spinlock architecture specific functions to the corresponding
 * queued spinlock functions.
 */
#define arch_spin_is_locked(l)		komb_spin_is_locked(l)
#define arch_spin_is_contended(l)	komb_spin_is_contended(l)
#define arch_spin_value_unlocked(l)	komb_spin_value_unlocked(l)
#define arch_spin_lock(l)		komb_spin_lock(l)
#define arch_spin_trylock(l)		komb_spin_trylock(l)
#define arch_spin_unlock(l)		komb_spin_unlock(l)

#endif /* __ASM_GENERIC_QSPINLOCK_H */
