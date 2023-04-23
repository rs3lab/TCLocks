#!/bin/bash

source ../../defaults.sh

locks=(table_spinlock table_aqs table_cna table_komb)
lock_name=(table_spinlock table_aqs table_cna table_komb)

lock_type=spinlock

rw_writes=(100)
rw_total=100
buckets=(1024)
entries_ratios=(4)

time=${runtime}

parent_dir=${results_dir}

DIR=${parent_dir}/results-${lock_type}-${ncores}cores-${time}seconds

make clean
make || exit

numlocks=${#locks[@]}

for bucket in ${buckets[@]}
do
for((i=0; i<$numlocks; i++))
do
	l=${locks[i]}
	for entry_ratio in ${entries_ratios[@]}
	do
		entries=$((${bucket}*${entry_ratio}))
		for write in ${rw_writes[@]}
		do
			out_dir=${DIR}/${bucket}buckets-${entries}entries/${run}/${lock_name[i]}/${write}percent_writes
			mkdir -p ${out_dir}
			for c in ${cores[@]}
			do
				echo "lock: ${l} threads: ${c} rw_ratio: ${write} entries: ${entries} buckets: ${bucket}"
				sudo dmesg -C
				sudo insmod build/ht.ko reader_type=$l writer_type=$l \
					ro=0 rw=$c \
					rw_writes=${write} rw_total=${rw_total} \
					buckets=${bucket} \
					entries=${entries}
				sleep ${time}
				sudo rmmod ht
				sleep 1
				sudo dmesg > ${out_dir}/core.${c}
				sleep 5
			done
		done
	done
done
done
