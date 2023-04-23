#ifndef _LINUX_KOMB_MUTEX_H
#define _LINUX_KOMB_MUTEX_H

#define SIZE_OF_SHADOW_STACK 8192L
#ifdef KERNEL_SYNCSTRESS
#include "lib/combiner.h"
#else
#endif

struct komb_mutex_node {
	struct komb_mutex_node *next;
	struct komb_mutex_node *next_rwsem;
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
};

struct komb_mutex {
	/* These two will be sufficient to design simple komb lock */
	struct komb_mutex_node *tail;
	union {
		atomic_t state;
		struct {
			u8 locked;
		};
	};
	struct task_struct *combiner_task; //TODO: Merge locked bit with owner.
};

#define __KOMBMUTEX_INITIALIZER(lockname)             {                         \
		.tail = NULL, .state = ATOMIC_INIT(0), .combiner_task = NULL }  \

#define DEFINE_KOMBMUTEX(mname)                                                \
	struct komb_mutex mname = __KOMBMUTEX_INITIALIZER(mname)

#define _Q_COMPLETED_OFFSET (_Q_LOCKED_OFFSET + _Q_LOCKED_BITS)
#define _Q_COMPLETED_BITS 8
#define _Q_COMPLETED_MASK _Q_SET_MASK(COMPLETED)

void komb_mutex_init(struct komb_mutex *lock);

static inline bool komb_mutex_is_locked(struct komb_mutex *lock)
{
	return !!READ_ONCE(lock->locked);
}

void komb_mutex_lock(struct komb_mutex *lock);
void komb_mutex_lock_rwsem_writer(struct komb_mutex *lock);
int __must_check kombmutex_lock_interruptible(struct komb_mutex *lock);
int __must_check kombmutex_lock_killable(struct komb_mutex *lock);
void komb_mutex_lock_io(struct komb_mutex *lock);

#define komb_mutex_lock_nested(lock, subclass) komb_mutex_lock(lock)
#define komb_mutex_lock_interruptible_nested(lock, subclass)                   \
	kombmutex_lock_interruptible(lock)
#define komb_mutex_lock_killable_nested(lock, subclass)                        \
	kombmutex_lock_killable(lock)
#define komb_mutex_lock_nest_lock(lock, nest_lock) kombmutex_lock(lock)
#define komb_mutex_lock_io_nested(lock, subclass) kombmutex_lock(lock)

int komb_mutex_trylock(struct komb_mutex *lock);
void komb_mutex_unlock(struct komb_mutex *lock);

#endif /* _LINUX_KOMB_MUTEX_H */
