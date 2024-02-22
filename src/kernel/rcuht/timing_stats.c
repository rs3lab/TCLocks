#include "timing_stats.h"

const char *Timingstring_locktime[TIMING_NUM] = { "write_critical_section",
						  "write_path", "combiner_loop",
						  "combiner_loop_lockfn", "combiner_loop_unlockfn",
						  "lock_stack_switch",
						  "unlock_stack_switch" };

unsigned long Timingstats_locktime[TIMING_NUM];
DEFINE_PER_CPU(unsigned long[TIMING_NUM], Timingstats_percpu_locktime);
unsigned long Countstats_locktime[TIMING_NUM];
DEFINE_PER_CPU(unsigned long[TIMING_NUM], Countstats_percpu_locktime);
unsigned long MinTimingstats_locktime[TIMING_NUM];
DEFINE_PER_CPU(unsigned long[TIMING_NUM], MinTimingstats_percpu_locktime);
unsigned long MaxTimingstats_locktime[TIMING_NUM];
DEFINE_PER_CPU(unsigned long[TIMING_NUM], MaxTimingstats_percpu_locktime);
unsigned long VarTimingstats_locktime[TIMING_NUM];
DEFINE_PER_CPU(unsigned long[TIMING_NUM], VarTimingstats_percpu_locktime);

// Used for radix sort
unsigned long* BucketTimingsamples_bucket[TIMING_NUM];
// 50% 75% 90% 99% 99.99% 
unsigned long BucketTimingstats_locktime[TIMING_NUM][5];
DEFINE_PER_CPU(unsigned long* [TIMING_NUM], BucketTimingstats_percpu_locktime);
DEFINE_PER_CPU(unsigned long[TIMING_NUM], bucket_counter);

DEFINE_PER_CPU(bool, do_timing);

static void locktime_get_timing_stats(void)
{
	int i, j;
	int cpu;
	uint64_t max_value, min_value;
	uint64_t sampled_number;
	uint64_t per50, update50, per75, update75, per90, update90, per99, update99, per9999, update9999;	

	for (i = 0; i < TIMING_NUM; i++) {
		Timingstats_locktime[i] = 0;
		Countstats_locktime[i] = 0;
		MinTimingstats_locktime[i] = ULONG_MAX;
		MaxTimingstats_locktime[i] = 0;
		VarTimingstats_locktime[i] = 0;
		for_each_possible_cpu (cpu) {
			/*unsigned long mean1, mean2, var1, var2, count1, count2;
			count1 = Countstats_locktime[i];
			count2 = per_cpu(Countstats_percpu_locktime[i], cpu);
			if(count1 == 0)
			{
				
			}else if(count2 == 0)
			{
			}
			else
			{
			}*/

			Timingstats_locktime[i] +=
				per_cpu(Timingstats_percpu_locktime[i], cpu);
			Countstats_locktime[i] +=
				per_cpu(Countstats_percpu_locktime[i], cpu);
			if (MinTimingstats_locktime[i] >
			    per_cpu(MinTimingstats_percpu_locktime[i], cpu))
				MinTimingstats_locktime[i] = per_cpu(
					MinTimingstats_percpu_locktime[i], cpu);
			if (MaxTimingstats_locktime[i] <
			    per_cpu(MaxTimingstats_percpu_locktime[i], cpu))
				MaxTimingstats_locktime[i] = per_cpu(
					MaxTimingstats_percpu_locktime[i], cpu);
		}
		//printk(KERN_ALERT "======== Update Sample Bucket ========\n");
	    	max_value = 0;
		min_value = -1;
		for_each_possible_cpu (cpu) {
			unsigned long loop_scope = (N_BUCKETS < per_cpu(bucket_counter[i], cpu)) ? N_BUCKETS: per_cpu(bucket_counter[i], cpu);
			for (j = 0; j < loop_scope; j++){
				// radix sort
				BucketTimingsamples_bucket[i][(per_cpu(BucketTimingstats_percpu_locktime[i],cpu)[j])] ++;
				max_value = (per_cpu(BucketTimingstats_percpu_locktime[i],cpu)[j] > max_value) ? per_cpu(BucketTimingstats_percpu_locktime[i],cpu)[j] : max_value;
				min_value = (per_cpu(BucketTimingstats_percpu_locktime[i],cpu)[j] < min_value) ? per_cpu(BucketTimingstats_percpu_locktime[i],cpu)[j] : min_value;
			}
		}
		//printk(KERN_ALERT "======== Update Sample Bucket Finished. Max: %lu Min: %lu========\n", max_value, min_value);
		sampled_number = 0;
		for (j = 0; j < TIME_UPPER_BOUND; j++){
			sampled_number += BucketTimingsamples_bucket[i][j];
		}
		//printk(KERN_ALERT "======== Get Total Sample Number %lu and Counter %lu ========\n", sampled_number, Countstats_locktime[i]);
		 per50 = sampled_number >> 1;
		 update50 = 1;
		 per75 = sampled_number >> 2;
		 update75 = 1;
		 per90 = sampled_number / 10;
		 update90 = 1;
		 per99 = sampled_number / 100;
		 update99 = 1;
		 per9999 = sampled_number / 10000;
		 update9999 = 1;
		 sampled_number = 0 ;
		//printk(KERN_ALERT "======== Calculate Percentile ========\n");
		for (j = TIME_UPPER_BOUND - 1; j >=0; j--){
			sampled_number += BucketTimingsamples_bucket[i][j];
			if (update50 == 1){
				if (sampled_number >= per50){
					BucketTimingstats_locktime[i][0] = j;
					update50 = 0;
				}
			}
			if (update75 == 1){
				if (sampled_number >= per75){
					BucketTimingstats_locktime[i][1] = j;
					update75 = 0;
				}
			}
			if (update90 == 1){
				if (sampled_number >= per90){
					BucketTimingstats_locktime[i][2] = j;
					update90 = 0;
				}
			}
			if (update99 == 1){
				if (sampled_number >= per99){
					BucketTimingstats_locktime[i][3] = j;
					update99 = 0;
				}
			}
			if (update9999 == 1){
				if (sampled_number >= per9999){
					BucketTimingstats_locktime[i][4] = j;
					update9999 = 0;
				}
			}
			if ((update50 | update75 | update90 | update99 | update9999) == 0) break;
		}
		//printk(KERN_ALERT "======== Calculate Percentile Finished ========\n");

	}
}

void locktime_print_timing_stats(void)
{
	int i;

	locktime_get_timing_stats();

	printk(KERN_ALERT "======== lock timing stats ========\n");
	for (i = 0; i < TIMING_NUM; i++) {
		if (Timingstats_locktime[i]) {
			printk(KERN_ALERT
			       "%s: count %lu timing %lu average %lu min %lu max %lu \nvar %lu 50-per %lu 75-per %lu 90-per %lu 99-per %lu 99.99-per %lu\n",
			       Timingstring_locktime[i], Countstats_locktime[i],
			       Timingstats_locktime[i],
			       Countstats_locktime[i] ?
				       Timingstats_locktime[i] /
					       Countstats_locktime[i] :
				       0,
			       MinTimingstats_locktime[i],
			       MaxTimingstats_locktime[i],
			       VarTimingstats_locktime[i],
			       BucketTimingstats_locktime[i][0],
			       BucketTimingstats_locktime[i][1],
			       BucketTimingstats_locktime[i][2],
			       BucketTimingstats_locktime[i][3],
			       BucketTimingstats_locktime[i][4]);
		} else {
			printk(KERN_ALERT "%s: count %lu\n",
			       Timingstring_locktime[i],
			       Countstats_locktime[i]);
		}
	}
}

void locktime_clear_stats(void)
{
	int i, j;
	int cpu;

	printk(KERN_ALERT "======== Clear lock timing stats ========\n");
	for (i = 0; i < TIMING_NUM; i++) {
		Countstats_locktime[i] = 0;
		Timingstats_locktime[i] = 0;
		for (j = 0; j < TIME_UPPER_BOUND; j++)
			BucketTimingsamples_bucket[i][j] = 0;
		for (j = 0; j < 5; j++)
			BucketTimingstats_locktime[i][j] = 0;
		//printk(KERN_ALERT "INITIALIZE Per CPU Data\n");

		for_each_possible_cpu (cpu) {

			per_cpu(Timingstats_percpu_locktime[i], cpu) = 0;
			per_cpu(Countstats_percpu_locktime[i], cpu) = 0;
			per_cpu(MinTimingstats_percpu_locktime[i], cpu) =
				ULONG_MAX;
			per_cpu(MaxTimingstats_percpu_locktime[i], cpu) = 0;
			per_cpu(VarTimingstats_percpu_locktime[i], cpu) = 0;
		        per_cpu(bucket_counter[i], cpu) = 0;
			//printk(KERN_ALERT "INITIALIZE Per CPU BucketTimingstats_percpu_locktime\n");
			for (j = 0; j < N_BUCKETS; j++)
				per_cpu(BucketTimingstats_percpu_locktime[i],
					cpu)[j] = 0;
		}
	}
}
