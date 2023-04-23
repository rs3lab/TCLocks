#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/topology.h>
#include <linux/compiler.h>
#include <linux/random.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/percpu.h>
#include <linux/atomic.h>
#include <asm/byteorder.h>
#include <linux/ktime.h>
#include <linux/vmalloc.h>
#include <linux/sort.h>

#include <asm/uaccess.h>

#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/rwlock.h>
#include <linux/percpu-rwsem.h>
#include <linux/percpu.h>
#include <linux/sched/clock.h>

#include "cpuseq.h"
#include "rlock_stresser.h"
#include "rangelock/excl_rlock.h"
#include "rangelock/rw_rlock.h"
#include "rangelock/segment_rlock.h"

#define RCU_RANDOM_MULT 39916801 /* prime */
#define RCU_RANDOM_ADD 479001701 /* prime */
#define RCU_RANDOM_REFRESH 10000

#define DEFINE_RCU_RANDOM(name) struct rcu_random_state name = { 0, 0 }

static long range_size = 1;
static int rw_writes = 1; //Number of writes out of total read+writes.
static int rw_total = 100; //Total read+write operations.
static int threads =
	-1; /* Number of mixed reader/writer threads; defaults to online CPUs */
static char *stress_type = "excl_rlock";
static int benchmark_type =
	0; // 0 -> same range, 1 -> disjoint range, 2 -> random range

module_param(stress_type, charp, S_IRUGO | S_IWUSR);
module_param(range_size, long, S_IRUGO | S_IWUSR);
module_param(rw_writes, int, S_IRUGO | S_IWUSR);
module_param(rw_total, int, S_IRUGO | S_IWUSR);
module_param(threads, int, 0444);
module_param(benchmark_type, int, S_IRUGO | S_IWUSR);

MODULE_PARM_DESC(threads, "Number of threads");

static struct stresser_context ctx = { 0, 0, ATOMIC_INIT(0), NULL };

static struct task_struct **tasks;
static struct thread_stats *thread_stats;
static char *shared_array;

struct rcu_random_state {
	unsigned long rrs_state;
	long rrs_count;
};

/*
 * Crude but fast random-number generator.  Uses a linear congruential
 * generator, with occasional help from cpu_clock().
 */
static unsigned long rcu_random(struct rcu_random_state *rrsp)
{
	if (--rrsp->rrs_count < 0) {
		rrsp->rrs_state +=
			(unsigned long)cpu_clock(raw_smp_processor_id());
		rrsp->rrs_count = RCU_RANDOM_REFRESH;
	}
	rrsp->rrs_state = rrsp->rrs_state * RCU_RANDOM_MULT + RCU_RANDOM_ADD;
	return swahw32(rrsp->rrs_state);
}

static inline void _rw_lines(long start, long end, bool write)
{
	long i, index;
	for (i = start; i < end; i++) {
		index = i * 64;
		int d = READ_ONCE(shared_array[index]);
		if (write)
			WRITE_ONCE(shared_array[index], d + i % 10);
		else
			(void)READ_ONCE(shared_array[index]);
	}
}

static int rlock_stress_ops(void *arg)
{
	struct thread_stats *tstat = arg;
	struct timespec64 start_t, end_t;

	long range_start, range_end, temp, nop_cnt;

	int tid = tstat->tid;

	bool print_range = true;

	DEFINE_RCU_RANDOM(rand);

	__synchronize_start(tstat->cid, ctx.state);
	ktime_get_raw_ts64(&start_t);
	do {
		if (need_resched())
			cond_resched();

		if (benchmark_type == 0) {
			range_start = 0;
			range_end = range_size - 1;
		} else if (benchmark_type == 1) {
			range_start =
				((range_size / threads) * tid) % range_size;
			range_end = (((range_size / threads) * (tid + 1)) - 1) %
				    range_size;
		} else {
			range_start = rcu_random(&rand) % range_size;
			range_end = rcu_random(&rand) % range_size;
		}

		if (range_end < range_start) {
			temp = range_start;
			range_start = range_end;
			range_end = temp;
		}

		if (print_range) {
			printk(KERN_ALERT "%d %ld %ld\n", tid, range_start,
			       range_end);
			print_range = false;
		}

		if ((rcu_random(&rand) % rw_total) < rw_writes) {
			void *node = ctx.ops->writelock(
				range_start, range_end - range_start);
			_rw_lines(range_start, range_end, true);
			ctx.ops->writeunlock(range_start,
					     range_end - range_start, node);
		} else {
			void *node = ctx.ops->readlock(range_start,
						       range_end - range_start);
			_rw_lines(range_start, range_end, false);
			ctx.ops->readunlock(range_start,
					    range_end - range_start, node);
		}

		tstat->jobs++;

		nop_cnt = rcu_random(&rand) % 2048;
		while (nop_cnt--)
			nop();

	} while (!kthread_should_stop() && !ctx.stop);
	ktime_get_raw_ts64(&end_t);

	end_t = timespec64_sub(end_t, start_t);
	tstat->time = timespec64_to_ns(&end_t);

	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);

	return 0;
}

//Exclusive range lock
DEFINE_LOCK_STR_OPS(excl_rlock, struct rlock, DEFINE_EXCL_RLOCK,
		    excl_rlock_init, NULL, rlock_stress_ops, excl_range_lock,
		    excl_range_unlock, excl_range_lock, excl_range_unlock);

//Read-Write range lock
DEFINE_LOCK_STR_OPS(rw_rlock, struct rglock, DEFINE_RW_RLOCK, rw_rlock_init,
		    rw_rlock_deinit, rlock_stress_ops, write_range_lock,
		    write_range_unlock, read_range_lock, read_range_unlock);

//Segment range lock
DEFINE_LOCK_STR_OPS(segment_rlock, struct range_lock, DEFINE_SEGMENT_RLOCK,
		    segment_range_lock_init, NULL, rlock_stress_ops,
		    segment_range_write_lock, segment_range_write_unlock,
		    segment_range_read_lock, segment_range_read_unlock);

static int cmp_tput(const void *ja, const void *jb)
{
	const struct thread_stats *a, *b;
	a = ja;
	b = jb;
	return (a->jobs == b->jobs) ? 0 : (a->jobs > b->jobs) ? 1 : -1;
}

static void rlock_stress_print_stats(void)
{
	int i;
	struct thread_stats s = {};
	struct thread_stats *cpu0_stats = &thread_stats[0];
	u64 min_job = cpu0_stats->jobs, max_job = cpu0_stats->jobs;
	u64 up = cpu0_stats->jobs, down = cpu0_stats->jobs, up_sum, down_sum;

	if (!thread_stats) {
		printk(KERN_ALERT "stress stats unavailable\n");
		return;
	}

	ctx.stop = 1;
	for (i = 0; i < threads; i++) {
		struct thread_stats *temp_stats = &thread_stats[i];
#ifdef PRINT_THREAD_STATS
		printk(KERN_ALERT "[%d]: jobs: %llu\n", i, temp_stats->jobs);
#endif
		s.jobs += temp_stats->jobs;
		if (s.time < temp_stats->time)
			s.time = temp_stats->time;
		if (temp_stats->jobs < min_job)
			min_job = temp_stats->jobs;
		if (temp_stats->jobs > max_job)
			max_job = temp_stats->jobs;
	}

	if (threads > 1) {
		sort(thread_stats, threads, sizeof(thread_stats[0]), cmp_tput,
		     NULL);
		up_sum = 0;
		down_sum = 0;

		for (i = 0; i < threads; ++i) {
			if (i < threads / 2)
				up_sum += thread_stats[i].jobs;
			else
				down_sum += thread_stats[i].jobs;
		}
		up = up_sum;
		down = down_sum;
	}

	printk(KERN_ALERT
	       "summary: threads=%d benchmark_type=%d type=%s rw_writes=%d rw_total=%d range_size=%ld\n summary: jobs: %llu (%llu) time: %llu min: %llu max: %llu ( %llu / %llu )\n",
	       threads, benchmark_type, stress_type, rw_writes, rw_total,
	       range_size, s.jobs, s.jobs / 1000000, s.time, min_job, max_job,
	       up, down);
}

static void rlock_stress_exit(void)
{
	int i, ret;

	printk(KERN_ALERT "rlock_stress_exit called \n");

	if (tasks) {
		ctx.stop = 1;
		for (i = 0; i < threads; i++)
			if (tasks[i]) {
				ret = kthread_stop(tasks[i]);
				if (ret)
					printk(KERN_ALERT
					       "rlock_stress task returned error %d\n",
					       ret);
			}
		kfree(tasks);
	}

	rlock_stress_print_stats();
	if (ctx.ops->deinit)
		ctx.ops->deinit();
	vfree(shared_array);
	kfree(thread_stats);
	printk(KERN_ALERT "rlock_stress done\n");
}

static __init int rlock_stress_init(void)
{
	int i, ret;

	static struct stresser_ops *stress_ops[] = {
		&stress_excl_rlock_ops, &stress_rw_rlock_ops,
		&stress_segment_rlock_ops
	};

	for (i = 0; i < ARRAY_SIZE(stress_ops); i++) {
		ctx.ops = stress_ops[i];
		if (strcmp(stress_type, ctx.ops->name) == 0)
			break;
	}

	if (i == ARRAY_SIZE(stress_ops)) {
		sync_log("invalid sync primitives specified");
		return -EINVAL;
	}

	if (ctx.ops->thread_ops == NULL)
		return -EINVAL;

	shared_array = vzalloc((range_size + 1) * 64);
	if (!shared_array)
		return -ENOMEM;

	if (threads < 0)
		threads = num_online_cpus();

	thread_stats =
		kcalloc(threads, sizeof(struct thread_stats), GFP_KERNEL);
	if (!thread_stats)
		return -ENOMEM;

	tasks = kcalloc(threads, sizeof(tasks[0]), GFP_KERNEL);
	if (!tasks)
		goto enomem;

	if (ctx.ops->init)
		ctx.ops->init();

	printk(KERN_ALERT "rlock stress starting threads\n");

	ctx.stop = 0;
	for (i = 0; i < threads; i++) {
		struct task_struct *task;
		struct thread_stats *temp = &(thread_stats[cpuseq[i]]);
		temp->cid = cpuseq[i] % online_cpus;
		temp->tid = i % online_cpus;
		task = kthread_create(ctx.ops->thread_ops, temp,
				      "rlock_stress");
		if (IS_ERR(task)) {
			ret = PTR_ERR(task);
			goto error;
		}
		tasks[i] = task;
		kthread_bind(tasks[i], cpuseq[i] % online_cpus);
		wake_up_process(tasks[i]);
	}

	return 0;
enomem:
	ret = -ENOMEM;
error:
	rlock_stress_exit();
	return ret;
}

module_init(rlock_stress_init);
module_exit(rlock_stress_exit);

MODULE_AUTHOR("Vishal Gupta <vishal.gupta@epfl.ch>");
MODULE_DESCRIPTION(
	"Simple stress testing for range locks. Acknowledgement: sync-stresser <Sanidhya Kashyap>, rcuht");
MODULE_LICENSE("GPL");
