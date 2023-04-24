#!/bin/bash

cd /home/ubuntu/TCLocks/src/benchmarks/vbench

./mkmounts tmpfs-separate

source /home/ubuntu/TCLocks/src/defaults.sh && export LD_PRELOAD=/usr/local/lib/libjemalloc.so.2 && ./config.py psearchy

sleep 5

./config.py metis

./mkmounts -u tmpfs-separate
