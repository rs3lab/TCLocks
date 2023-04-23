#!/bin/bash

source ../../defaults.sh

locks=(table_komb)
lock_name=(table_komb)

lock_type=spinlock

rw_writes=(100)
rw_total=100
buckets=1024
entries_ratios=(4)

optimization=(baseline numa numaprf numaprfwwjump)

time=${runtime}

parent_dir=${results_dir}

DIR=${parent_dir}/results-${lock_type}-${ncores}cores-${time}seconds

numlocks=${#locks[@]}

# Build baseline

sed -i 's/^#define NUMA_AWARE/\/\/#define NUMA_AWARE/' include/lib/combiner.h
sed -i 's/^#define PREFETCHING/\/\/#define PREFETCHING/' include/lib/combiner.h
sed -i 's/^#define WWJUMP/\/\/#define WWJUMP/' include/lib/combiner.h

make clean
make || exit

mv build/ht.ko ht-baseline.ko

# Build numa

sed -i 's/^\/\/#define NUMA_AWARE/#define NUMA_AWARE/' include/lib/combiner.h

make clean
make || exit

mv build/ht.ko ht-numa.ko

# Build numa + prf

sed -i 's/^\/\/#define PREFETCHING/#define PREFETCHING/' include/lib/combiner.h

make clean
make || exit

mv build/ht.ko ht-numaprf.ko

# Build numa + prf + wwjump

sed -i 's/^\/\/#define WWJUMP/#define WWJUMP/' include/lib/combiner.h

make clean
make || exit

mv build/ht.ko ht-numaprfwwjump.ko

for opti in ${optimization[@]}
do
	l=${locks[0]}
	for entry_ratio in ${entries_ratios[@]}
	do
		entries=$((${buckets}*${entry_ratio}))
		for write in ${rw_writes[@]}
		do
			out_dir=${DIR}/${buckets}buckets-${entries}entries/${run}/${lock_name[0]}-${opti}/${write}percent_writes
			mkdir -p ${out_dir}
			for c in ${cores[@]}
			do
				echo "lock: ${l} threads: ${c} rw_ratio: ${write} entries: ${entries} buckets: ${buckets} optimization: ${opti}"
				sudo dmesg -C
				sudo insmod ht-${opti}.ko reader_type=$l writer_type=$l \
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

rm *.ko
