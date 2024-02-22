#ifndef __SWILOCK_H__
#define __SWILOCK_H__

#include "qspinlock.h"
#include "komb.h"

// Delegation settings
#define MAX_CORES 20
#define CORES_PER_SOCKET 20
#define ENABLE_JUMP 1       // Context switch from client to client
#define ENABLE_PREFETCH 1   // Prefetch before context switch

#define DEBUG_DELEGATION 0

#define DEFINE_SWILOCK(x)                                                      \
	struct orig_qspinlock(x) = (struct orig_qspinlock)__ORIG_QSPIN_LOCK_UNLOCKED

struct swi_node {
	struct swi_node *next;
    struct komb_node *komb_node;
    struct mcs_spinlock *qspin_node;
    bool is_delegate;  // Used by delegation, to ensure unlock with delegate_finish()
	char dummy1[36];

	bool active;       // Write only by client thread, read only by helper thread
	char dummy2[127];
};
extern struct swi_node swi_nodes[MAX_NODES]; 

void swilock_qnodes_init(void);
void swilock_qnodes_free(void);

void swilock_helper_init(void);
void swilock_helper_exit(void);

extern struct swi_node *swi_decode_tail(u32 tail);
extern void swi_notify_next(struct swi_node *swi_node);

extern void swilock_init(struct qspinlock *lock);
extern void swi_lock(struct qspinlock *lock);
extern void swi_unlock(struct qspinlock *lock);

/********************************************************************************
 * Delegation
 * ******************************************************************************/

struct delegation_request {
	void *base_ptr;             // 8 points to the base of the shadow stack.
	void* main_stack_ptr;    // 8 client main stack
    	void* shadow_stack_ptr;  // 8 client shadow stack
	int cpu_id;                 // 4
    	int socket_id;              // 4
    	int cpu_id_on_socket;       // 4
	bool toggle;                // 1 client write only; request active when != request toggle
	char dummy[91];             // pad to 128-byte
};

extern struct delegation_request delegation_requests; 

struct response{
	u64 toggle;
	char dummy[120];
};

struct delegation_server{
	struct response* responses[MAX_CORES / CORES_PER_SOCKET]; // 1 toggle bit for each thread
	struct qspinlock *lock;
	void* server_stack_ptr;
	int cur_client_cpu_id;
	char dummy[100];
	// for client->client jump
	int cur_updated_response;
	int cur_socket_id;
	int cur_client_cpu_id_on_socket;
};
extern struct delegation_server *server_ptr;

extern void swilock_delegate_init(struct qspinlock *lock);
extern void swilock_delegate_exit(void);

extern void swilock_delegate(struct qspinlock *lock);
extern void swilock_delegate_finish(struct qspinlock *lock);

#endif
