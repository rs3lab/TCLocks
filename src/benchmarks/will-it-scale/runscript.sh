#!/bin/bash

source ../../defaults.sh

workload=(lock1 mmap1)

#workload=(brk1 context_switch1 dup1 eventfd1 fallocate1 fallocate2 futex1 futex2 futex3 futex4 getppid1 lock1 lock2 lseek1 lseek2 malloc1 malloc2 mmap1 mmap2 open1 open2 open3 page_fault1 page_fault2 page_fault3 pipe1 poll1 poll2 posix_semaphore1 pread1 pread2 pread3 pthread_mutex1 pthread_mutex2 pthread_mutex3 pthread_mutex4 pwrite1 pwrite2 pwrite3 read1 read2 read3 read4 readseek1 readseek2 readseek3 sched_yield signal1 tlb_flush1 tlb_flush2 tlb_flush3 unix1 unlink1 unlink2 write1 writeseek1 writeseek2 writeseek3)

ncore=${ncores}

parent_folder=${results_dir}/will-it-scale

output_folder=${parent_folder}/${kernel}-${ncore}/

kernel_version=`uname -r`

if [[ "$kernel_version" != *"$kernel"* ]]; then
	echo "Incorrect Kernel"
	exit
fi

mkdir -p ${output_folder}

mkdir -p /tmp

make -j4

sudo mount -t tmpfs -o size=10G tmpfs /tmp

for wl in ${workload[@]}
do
	sudo mount -t tmpfs -o size=10G tmpfs /tmp/

	echo "# `uname -r` : $wl"
	./runtest.py $wl > ${output_folder}/$wl.log
	
	sudo umount /tmp/

	sleep 5
done
sudo umount /tmp
