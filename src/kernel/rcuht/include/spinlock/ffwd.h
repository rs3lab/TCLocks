#ifndef __FFWD_H__
#define __FFWD_H__

#include "qspinlock.h"
#include "komb.h"

// Delegation settings
#define MAX_CORES 48
#define CORES_PER_SOCKET 48
#define DELEGATION_CPU 47
#define ENABLE_JUMP 1       // Context switch from client to client
#define ENABLE_PREFETCH 1   // Prefetch before context switch

#define DEBUG_DELEGATION 0
#define FFWD_STATS 1

#define CACHELINE_ALIGNEMENT 64

#define DEFINE_FFWD(x)                                                      \
	struct orig_qspinlock(x) = (struct orig_qspinlock)__ORIG_QSPIN_LOCK_UNLOCKED

struct ffwd_node {
	struct ffwd_node *next;
    struct komb_node *komb_node;
    struct mcs_spinlock *qspin_node;
    bool is_delegate;  // Used by delegation, to ensure unlock with delegate_finish()
	char dummy1[36];

	bool active;       // Write only by client thread, read only by helper thread
	char dummy2[127];
};
extern struct ffwd_node ffwd_nodes[MAX_NODES]; 

void ffwd_qnodes_init(void);
void ffwd_qnodes_free(void);

void ffwd_helper_init(void);
void ffwd_helper_exit(void);

extern struct ffwd_node *ffwd_decode_tail(u32 tail);
extern void ffwd_notify_next(struct ffwd_node *ffwd_node);

extern void ffwd_init(struct qspinlock *lock);
extern void ffwd_lock(struct qspinlock *lock);
extern void ffwd_unlock(struct qspinlock *lock);

#if FFWD_STATS
extern void ffwd_print_stats(void);
#endif

/********************************************************************************
 * Delegation
 * ******************************************************************************/

struct delegation_request {
	union {
		struct {
			void *base_ptr;             // 8 points to the base of the shadow stack.
			void* main_stack_ptr;    // 8 client main stack
			void* shadow_stack_ptr;  // 8 client shadow stack
			int cpu_id;                 // 4
			int socket_id;              // 4
			int cpu_id_on_socket;       // 4
		};
		char alignment[CACHELINE_ALIGNEMENT];             // pad to 128-byte
	};
	union {
		bool toggle;                // 1 client write only; request active when != request toggle
		char alignment1[CACHELINE_ALIGNEMENT];
	};
};

extern struct delegation_request delegation_requests; 

struct response {
	union {
		bool toggle;
		char alignment[CACHELINE_ALIGNEMENT];
	};
} ____cacheline_aligned;

struct delegation_server {
	union {
		struct {
			struct qspinlock *lock;
			void* server_stack_ptr;
			int cur_client_cpu_id;
			int prev_client_cpu_id;
			int cur_updated_response;
			int cur_socket_id;
			int cur_client_cpu_id_on_socket;
			int waiters_combined;
		};
		char alignment[CACHELINE_ALIGNEMENT];
	};
	struct response responses[MAX_CORES]; // 1 toggle bit for each thread
};

extern void ffwd_delegate_init(struct qspinlock *lock);
extern void ffwd_delegate_exit(void);

//extern void ffwd_delegate(struct qspinlock *lock);
//extern void ffwd_delegate_finish(struct qspinlock *lock);

#endif
