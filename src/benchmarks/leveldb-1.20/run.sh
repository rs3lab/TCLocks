#!/bin/bash

# $1 --> number of threads
# $2 --> millisecond
#db_bench --benchmarks=readrandom --threads=$1 --num=10 --time_ms=$2 2>&1 | grep readrandom
set -x
LOCK_DIR=./../../ulocks/src/litl

LOCKS=(libucomb_spinlock.sh libcna_spinlock.sh libaqswonode_spinlock.sh)

DIR=results

c=$1
for l in ${LOCKS[@]}
do
	mkdir -p ${DIR}/$l
        ${LOCK_DIR}/${l} taskset -c 0-$(($c - 1)) ./out-static/db_bench --benchmarks=readrandom --num=20 --time_ms=$2 --threads=$c >> ${DIR}/${l}/core.${c}
done
