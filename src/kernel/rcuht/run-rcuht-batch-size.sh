#!/bin/bash

source ../../defaults.sh

locks=(table_komb)
lock_name=(table_komb)

lock_type=spinlock

rw_writes=(100)
rw_total=100
buckets=1024
entries_ratios=(4)

komb_batch_sizes=(65536) #(1024 4096 16384 65536)

time=${runtime}

parent_dir=${results_dir}

DIR=${parent_dir}/results-${lock_type}-${ncores}cores-${time}seconds

make clean
make || exit

numlocks=${#locks[@]}

for komb_batch_size in ${komb_batch_sizes[@]}
do
	l=${locks[0]}
	for entry_ratio in ${entries_ratios[@]}
	do
		entries=$((${buckets}*${entry_ratio}))
		for write in ${rw_writes[@]}
		do
			out_dir=${DIR}/${buckets}buckets-${entries}entries/${run}/${lock_name[i]}-${komb_batch_size}batchsize/${write}percent_writes
			mkdir -p ${out_dir}
			for c in ${cores[@]}
			do
				echo "lock: ${l} threads: ${c} rw_ratio: ${write} entries: ${entries} buckets: ${buckets} batch_size: ${komb_batch_size}"
				sudo dmesg -C
				sudo insmod build/ht.ko reader_type=$l writer_type=$l \
					ro=0 rw=$c \
					rw_writes=${write} rw_total=${rw_total} \
					buckets=${buckets} \
					entries=${entries} komb_batch_size=${komb_batch_size}
				sleep ${time}
				sudo rmmod ht
				sleep 1
				sudo dmesg > ${out_dir}/core.${c}
				sleep 5
			done
		done
	done
done
