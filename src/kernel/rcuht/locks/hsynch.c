#include "spinlock/hsynch.h"

csynch_node_t *hsynch_tail, *hsynch_level2tail;
thread_data_t *hsynch_thread_data;
csynch_node_t *level2_nodes[NUM_NUMA_NODES];
bool ttas_lock_var = false;

void (*hsynch_critical_section)(int, bool) = NULL;
int hsynch_cs_arg1 = 0; //dirty_clines
bool hsynch_cs_arg2 = false; //is_dirty

#ifdef MCS_LOCK
void acquire_mcs_lock(csynch_node_t *my_qnode)
{
	WRITE_ONCE(my_qnode->next, NULL);
	csynch_node_t *prev_qnode = xchg(&hsynch_level2tail, my_qnode);
	if (READ_ONCE(prev_qnode) != NULL) {
		WRITE_ONCE(my_qnode->wait, true);
		WRITE_ONCE(prev_qnode->next, my_qnode);
		while (READ_ONCE(my_qnode->wait) == true)
			smp_rmb();
	}
}

void release_mcs_lock(csynch_node_t *my_qnode)
{
	if (READ_ONCE(my_qnode->next) == NULL) {
		if (cmpxchg(&hsynch_level2tail, my_qnode, NULL) == false) {
			while (READ_ONCE(my_qnode->next) == NULL)
				smp_rmb();
			WRITE_ONCE(my_qnode->next->wait, false);
		}
	}
}
#endif

#ifdef TTAS_LOCK
void acquire_ttas_lock(void)
{
	int time = 1;
	do {
		//this_thread::sleep_for(chrono::milliseconds(time));
		time *= 2;
		while (ttas_lock_var == true)
			cpu_relax();
	} while (smp_cas_bool(&ttas_lock_var, false, true) == false);
}

void release_ttas_lock(void)
{
	ttas_lock_var = false;
}
#endif

void hsynch_execute(int cid)
{
	int counter = 0;
	int maxRequestProcess = 2048;

	unsigned int nodeid;
	nodeid = cid % 2 ? 1 : 0;

	volatile csynch_node_t *curNode, *tmpNode, *tmpNodeNext, *nextNode;

	nextNode = hsynch_thread_data[cid].myNodes[0];

	WRITE_ONCE(nextNode->next, NULL);
	WRITE_ONCE(nextNode->wait, true);
	WRITE_ONCE(nextNode->completed, false);

	smp_wmb();

	curNode = xchg(&hsynch_tail, nextNode);

	WRITE_ONCE(curNode->next, nextNode);
	hsynch_thread_data[cid].myNodes[0] = curNode;

	while (READ_ONCE(curNode->wait))
		smp_rmb();

	if (READ_ONCE(curNode->completed))
		return;

#if defined(MCS_LOCK)
	acquire_mcs_lock(level2_nodes[nodeid]);
#elif defined(TTAS_LOCK)
	acquire_ttas_lock();
#endif

	tmpNode = curNode;
	while (READ_ONCE(tmpNode->next) != NULL &&
	       counter < maxRequestProcess) {
		counter++;

		tmpNodeNext = READ_ONCE(tmpNode->next);

		hsynch_critical_section(hsynch_cs_arg1, hsynch_cs_arg2);

		WRITE_ONCE(tmpNode->completed, true);
		smp_wmb();
		WRITE_ONCE(tmpNode->wait, false);
		tmpNode = tmpNodeNext;

		if (need_resched())
			break;
	}

#if defined(MCS_LOCK)
	release_mcs_lock(level2_nodes[nodeid]);
#elif defined(TTAS_LOCK)
	release_ttas_lock();
#endif

	WRITE_ONCE(tmpNode->wait, false);

	smp_wmb();

	return;
}

void hsynch_init(int threads)
{
	int i = 0, j = 0;
	csynch_node_t *temp =
		(csynch_node_t *)kmalloc(sizeof(csynch_node_t), GFP_KERNEL);
	temp->wait = false;
	temp->completed = false;
	temp->next = NULL;
	smp_mb();
	hsynch_tail = temp;
	hsynch_level2tail = NULL;
	for (; i < NUM_NUMA_NODES; i++) {
		level2_nodes[i] = (csynch_node_t *)kmalloc(
			sizeof(csynch_node_t), GFP_KERNEL);
		level2_nodes[i]->wait = false;
		level2_nodes[i]->completed = false;
		level2_nodes[i]->next = NULL;
	}
	smp_mb();

	hsynch_thread_data = (thread_data_t *)kmalloc(
		threads * sizeof(thread_data_t), GFP_KERNEL);

	for (i = 0; i < threads; i++) {
		hsynch_thread_data[i].myNodes =
			(volatile csynch_node_t **)kmalloc(
				NODES_PER_THREAD * sizeof(csynch_node_t *),
				GFP_KERNEL);
		for (j = 0; j < NODES_PER_THREAD; j++)
			hsynch_thread_data[i].myNodes[j] =
				(csynch_node_t *)kmalloc(sizeof(csynch_node_t),
							 GFP_KERNEL);
	}
}
