#!/bin/bash

source ../../defaults.sh

workload=(lock1 mmap1)

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

make

sudo mount -t tmpfs -o size=10G tmpfs /tmp

for wl in ${workload[@]}
do
	sudo mount -t tmpfs -o size=10G tmpfs /tmp/

	echo "# `uname -r` : $wl"
	./runtest.py $wl > ${output_folder}/$wl.log
	
	sudo umount /tmp/
done
sudo umount /tmp
