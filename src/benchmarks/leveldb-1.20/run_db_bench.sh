#!/bin/bash

source ../../defaults.sh 

cnt=${ncores}

parent_folder=${results_dir}/leveldb
result_dir=${parent_folder}/${kernel}-${cnt}
DB_BENCH=./out-static/db_bench
DB_PATH=/tmp/db

litl_path=../../userspace/litl/

litl_lock=("" ${litl_path}/libaqswonode_spinlock.sh ${litl_path}/libkomb_spinlock.sh)
lock_name=(stock shfllock komb)

#Build if DB_BENCH does not exist
if [ ! -f $DB_BENCH ]; then
	cd jemalloc/
	./autogen.sh
	./configure --without-export --disable-libdl
	make -j`nproc`
	cd ../

	./build_detect_platform build_config.mk .
	make -j`nproc`
fi

# Create result directory
mkdir -p ${DB_PATH}

for ((i=0;i<${#lock_name[@]};i++))
do
for c in ${cores[@]}
do
	lock=${litl_lock[$i]}
	echo ${lock} ${c}
	result_dir=${parent_folder}/${lock_name[$i]}-${cnt}
	mkdir -p $result_dir
	sudo mount -t tmpfs -o size=10G tmpfs $DB_PATH
	# Create DB with db_bench
	$DB_BENCH --db=$DB_PATH --benchmarks=fillseq --threads=1
	# sync
	echo $c

	taskset -c 0-$(($c-1)) $lock $DB_BENCH --pin_threads=1 --benchmarks=readrandom --use_existing_db=1 --db=$DB_PATH --threads=$c --time_ms=30000 | tee $result_dir/readrandom.$c

	sudo rm -rf $DB_PATH/*
	sudo umount $DB_PATH
	sleep 5
done
done
