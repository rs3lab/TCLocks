#!/bin/bash

make
source /home/ubuntu/TCLocks/src/defaults.sh
echo ${cores}
bin/run-fxmark.py
