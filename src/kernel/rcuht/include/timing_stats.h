/*
 * locktime_stats.h
 *
 *  Created on: Sep 13, 2021
 *      Author: dzhou
 *      Modified By: vishal
 */

#ifndef __TIMING_STATS_H_
#define __TIMING_STATS_H_

#include <asm/msr.h>
#include <linux/cpu.h>

#define LOCK_MEASURE_TIME 1
#define N_BUCKETS 1048576
#define TIME_UPPER_BOUND 1048576 //131072

static inline unsigned long locktime_timing_start(void)
{
	unsigned long rax, rdx;
	__asm__ __volatile__("rdtscp\n" : "=a"(rax), "=d"(rdx) : : "%ecx");
	return (rdx << 32) + rax;
}

static inline unsigned long locktime_timing_end(void)
{
	unsigned long rax, rdx;
	__asm__ __volatile__("rdtscp\n" : "=a"(rax), "=d"(rdx) : : "%ecx");
	return (rdx << 32) + rax;
}

enum timing_category {
	write_critical_section_t,
	write_path_t,
	combiner_loop_t,
	combiner_loop_lockfn_t,
	combiner_loop_unlockfn_t,
	lock_stack_switch_t, //Waiter switch from Main->Ephemeral
	unlock_stack_switch_t, //Waiter switch from Ephemeral->Main

	TIMING_NUM,
};

extern const char *Timingstring_locktime[TIMING_NUM];

extern unsigned long Timingstats_locktime[TIMING_NUM];
DECLARE_PER_CPU(unsigned long[TIMING_NUM], Timingstats_percpu_locktime);

extern unsigned long BucketTimingstats_locktime[TIMING_NUM][5];
extern unsigned long* BucketTimingsamples_bucket[TIMING_NUM];
DECLARE_PER_CPU(unsigned long[TIMING_NUM], bucket_counter);
DECLARE_PER_CPU(unsigned long* [TIMING_NUM],
		BucketTimingstats_percpu_locktime);

extern unsigned long Countstats_locktime[TIMING_NUM];
DECLARE_PER_CPU(unsigned long[TIMING_NUM], Countstats_percpu_locktime);

extern unsigned long MinTimingstats_locktime[TIMING_NUM];
DECLARE_PER_CPU(unsigned long[TIMING_NUM], MinTimingstats_percpu_locktime);

extern unsigned long MaxTimingstats_locktime[TIMING_NUM];
DECLARE_PER_CPU(unsigned long[TIMING_NUM], MaxTimingstats_percpu_locktime);

extern unsigned long VarTimingstats_locktime[TIMING_NUM]; //Variance
DECLARE_PER_CPU(unsigned long[TIMING_NUM], VarTimingstats_percpu_locktime);

DECLARE_PER_CPU(bool, do_timing);

typedef unsigned long timing_t;

extern void locktime_print_timing_stats(void);

extern void locktime_clear_stats(void);

static inline void update_timing_stats(unsigned long diff, int name)
{
	/*if ((*this_cpu_ptr(&Countstats_percpu_locktime[name])) < 1000) {
		__this_cpu_add(Countstats_percpu_locktime[name], 1);
		return;
	}*/

	if (diff >= TIME_UPPER_BOUND)
		return;

	if ((*this_cpu_ptr(&MinTimingstats_percpu_locktime[name])) > diff)
		*this_cpu_ptr(&MinTimingstats_percpu_locktime[name]) = diff;
	if ((*this_cpu_ptr(&MaxTimingstats_percpu_locktime[name])) < diff)
		*this_cpu_ptr(&MaxTimingstats_percpu_locktime[name]) = diff;
	__this_cpu_add(Timingstats_percpu_locktime[name], diff);
	__this_cpu_add(Countstats_percpu_locktime[name], 1);

	(*this_cpu_ptr(&BucketTimingstats_percpu_locktime[name]))[(*this_cpu_ptr(&bucket_counter[name])) % N_BUCKETS] = diff;
	__this_cpu_add(bucket_counter[name], 1);
	//if (diff > 100000L)
	//	__this_cpu_add(BucketTimingstats_percpu_locktime[name][4], 1);
	//else if (diff > 10000L)
	//	__this_cpu_add(BucketTimingstats_percpu_locktime[name][3], 1);
	//else if (diff > 1000L)
	//	__this_cpu_add(BucketTimingstats_percpu_locktime[name][2], 1);
	//else if (diff > 100L)
	//	__this_cpu_add(BucketTimingstats_percpu_locktime[name][1], 1);
	//else
	//	__this_cpu_add(BucketTimingstats_percpu_locktime[name][0], 1);

	/*long delta, delta2, mean, count, var;
	count = *this_cpu_ptr(&Countstats_percpu_locktime[name]);
	mean = (*this_cpu_ptr(&Timingstats_percpu_locktime[name])) / count;
	var = *this_cpu_ptr(&VarTimingstats_percpu_locktime[name]);
	delta = diff - mean;
	mean += (delta / count);
	delta2 = diff - mean;
	var += delta * delta2;
	*this_cpu_ptr(&VarTimingstats_percpu_locktime[name]);*/
}

#if LOCK_MEASURE_TIME

#define LOCK_DEFINE_TIMING_VAR(name) volatile timing_t name

#define LOCK_START_TIMING_DISABLE(name, start)
#define LOCK_START_TIMING_PER_CPU_DISABLE(name)
#define LOCK_END_TIMING_DISABLE(name, start)
#define LOCK_END_TIMING_PER_CPU_DISABLE(name)

#define LOCK_START_TIMING(name, start)                                         \
	do {                                                                   \
		if (*this_cpu_ptr(&do_timing)) {                               \
			barrier();                                             \
			start = locktime_timing_start();                       \
			barrier();                                             \
		}                                                              \
	} while (0)

#define LOCK_START_TIMING_PER_CPU(name)                                        \
	do {                                                                   \
		if (*this_cpu_ptr(&do_timing)) {                               \
			barrier();                                             \
			*this_cpu_ptr(&name) = locktime_timing_start();        \
			barrier();                                             \
		}                                                              \
	} while (0)

#define LOCK_END_TIMING(name, start)                                           \
	do {                                                                   \
		if (*this_cpu_ptr(&do_timing)) {                               \
			timing_t end;                                          \
			barrier();                                             \
			end = locktime_timing_end();                           \
			barrier();                                             \
			update_timing_stats((end - start), name);              \
		}                                                              \
	} while (0)

#define LOCK_END_TIMING_PER_CPU(name)                                          \
	do {                                                                   \
		if (*this_cpu_ptr(&do_timing)) {                               \
			timing_t end;                                          \
			barrier();                                             \
			end = locktime_timing_end();                           \
			barrier();                                             \
			update_timing_stats((end - (*this_cpu_ptr(&name))),    \
					    name##_t);                         \
		}                                                              \
	} while (0)
#else

#define LOCK_START_TIMING_DISABLE(name, start)
#define LOCK_START_TIMING_PER_CPU_DISABLE(name)
#define LOCK_END_TIMING_DISABLE(name, start)
#define LOCK_END_TIMING_PER_CPU_DISABLE(name)

#define LOCK_DEFINE_TIMING_VAR(name)

#define LOCK_START_TIMING(name, start)
#define LOCK_START_TIMING_PER_CPU(name)

#define LOCK_END_TIMING(name, start)
#define LOCK_END_TIMING_PER_CPU(name)
#endif

#endif /* __TIMING_STATS_H_ */
