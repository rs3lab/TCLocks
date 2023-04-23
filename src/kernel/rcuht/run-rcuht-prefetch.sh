#!/bin/bash

source ../../defaults.sh

locks=(table_komb)
lock_name=(table_komb)

lock_type=spinlock

rw_writes=(100)
rw_total=100
buckets=1024
entries_ratios=(4)

prefetch_lines=(2 4 6 8)

time=${runtime}

parent_dir=${results_dir}

DIR=${parent_dir}/results-${lock_type}-${ncores}cores-${time}seconds

numlocks=${#locks[@]}

for prf_ln in ${prefetch_lines[@]}
do
	sed -i 's/NUM_PREFETCH_LINES 6/NUM_PREFETCH_LINES '${prf_ln}'/' include/lib/combiner.h 
	make clean
	make || exit
	l=${locks[0]}
	for entry_ratio in ${entries_ratios[@]}
	do
		entries=$((${buckets}*${entry_ratio}))
		for write in ${rw_writes[@]}
		do
			out_dir=${DIR}/${buckets}buckets-${entries}entries/${run}/${lock_name[0]}-${prf_ln}prefetchlines/${write}percent_writes
			mkdir -p ${out_dir}
			for c in ${cores[@]}
			do
				echo "lock: ${l} threads: ${c} rw_ratio: ${write} entries: ${entries} buckets: ${buckets} prefetch_line: ${prf_ln}"
				sudo dmesg -C
				sudo insmod build/ht.ko reader_type=$l writer_type=$l \
					ro=0 rw=$c \
					rw_writes=${write} rw_total=${rw_total} \
					buckets=${buckets} \
					entries=${entries}
				sleep ${time}
				sudo rmmod ht
				sleep 1
				sudo dmesg > ${out_dir}/core.${c}
			done
		done
	done
done

sed -i 's/NUM_PREFETCH_LINES 6/NUM_PREFETCH_LINES 6/' include/lib/combiner.h 

