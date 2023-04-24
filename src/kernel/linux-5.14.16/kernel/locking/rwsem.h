/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The owner field of the rw_semaphore structure will be set to
 * RWSEM_READER_OWNED when a reader grabs the lock. A writer will clear
 * the owner field when it unlocks. A reader, on the other hand, will
 * not touch the owner field when it unlocks.
 *
 * In essence, the owner field now has the following 4 states:
 *  1) 0
 *     - lock is free or the owner hasn't set the field yet
 *  2) RWSEM_READER_OWNED
 *     - lock is currently or previously owned by readers (lock is free
 *       or not set by owner yet)
 *  3) RWSEM_ANONYMOUSLY_OWNED bit set with some other bits set as well
 *     - lock is owned by an anonymous writer, so spinning on the lock
 *       owner should be disabled.
 *  4) Other non-zero value
 *     - a writer owns the lock and other writers can spin on the lock owner.
 */

/*
 * Bit manipulation (not used currently)
 * Will use just one variable of 4 byts to enclose the following:
 * 0-7:   locked or unlocked
 * 8-15:  shuffle leader or not
 * 16-31: shuffle count
 */
#define _RWAQ_MCS_SET_MASK(type)  (((1U << _RWAQ_MCS_ ## type ## _BITS) -1)\
                                 << _RWAQ_MCS_ ## type ## _OFFSET)
#define _RWAQ_MCS_GET_VAL(v, type)   (((v) & (_RWAQ_MCS_ ## type ## _MASK)) >>\
                                    (_RWAQ_MCS_ ## type ## _OFFSET))
#define _RWAQ_MCS_LOCKED_OFFSET   0
#define _RWAQ_MCS_LOCKED_BITS     8
#define _RWAQ_MCS_LOCKED_MASK     _RWAQ_MCS_SET_MASK(LOCKED)
#define _RWAQ_MCS_LOCKED_VAL(v)   _RWAQ_MCS_GET_VAL(v, LOCKED)

#define _RWAQ_MCS_SLEADER_OFFSET  (_RWAQ_MCS_LOCKED_OFFSET + _RWAQ_MCS_LOCKED_BITS)
#define _RWAQ_MCS_SLEADER_BITS    8
#define _RWAQ_MCS_SLEADER_MASK    _RWAQ_MCS_SET_MASK(SLEADER)
#define _RWAQ_MCS_SLEADER_VAL(v)  _RWAQ_MCS_GET_VAL(v, SLEADER)

#define _RWAQ_MCS_WCOUNT_OFFSET   (_RWAQ_MCS_SLEADER_OFFSET + _RWAQ_MCS_SLEADER_BITS)
#define _RWAQ_MCS_WCOUNT_BITS     16
#define _RWAQ_MCS_WCOUNT_MASK     _RWAQ_MCS_SET_MASK(WCOUNT)
#define _RWAQ_MCS_WCOUNT_VAL(v)   _RWAQ_MCS_GET_VAL(v, WCOUNT)

#define _RWAQ_MCS_NOSTEAL_VAL     (1U << (_RWAQ_MCS_LOCKED_OFFSET + _RWAQ_MCS_LOCKED_BITS))

#define _RWAQ_MCS_STATUS_PARKED   0 /* node's status is changed to park */
#define _RWAQ_MCS_STATUS_PWAIT    1 /* starting point for everyone */
#define _RWAQ_MCS_STATUS_UNPWAIT  2 /* waiter is never scheduled out in this state */
#define _RWAQ_MCS_STATUS_LOCKED   4 /* node is now going to be the lock holder */
#define _AQ_MAX_LOCK_COUNT      256u
