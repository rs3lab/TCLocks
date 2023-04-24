#!/bin/bash

v=`sudo cat /sys/kernel/debug/kvm/halt_exits`
echo "$1 -- $v" >> /home/sanidhya/bench/halt-exits-dump
