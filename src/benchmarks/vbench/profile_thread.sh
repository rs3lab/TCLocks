#!/bin/bash

###  List of command line args (in order)
# 1. --bench    : name of the benchmark
#    options    : exim, psearchy, hist,
# 2. --mountfs  : filesystem to mount
#    options    : tmpfs-separate, tmpfs, hugetlb
#    default    : none, can be skipped
# 3. --expfolder: where to place experiments
#    default    : DDMMYYYY
#
#
##########################
helpFunction()
{
   echo ""
   echo "Usage: $0 -b benchmark -m mountfs -e result_dir -c #cores"
   echo -e "\t-b name of the benchmark. Options: psearchy, metis"
   echo -e "\t-m filesystem to mount. Options: tmpfs-separate, tmpfs, hugetlb"
   echo -e "\t-e the folder to save results"
   echo -e "\t-c the number of cores to run"
   exit 1 # Exit script after printing help
}

while getopts "b:m:e:c:" opt
do
   case "$opt" in
      b ) BENCH="$OPTARG" ;;
      m ) FS="$OPTARG" ;;
      e ) EXP_FOLDER_NAME="$OPTARG" ;;
      c ) N_CORES="$OPTARG" ;;
      ? ) helpFunction ;; # Print helpFunction in case parameter is non-existent
   esac
done

# Print helpFunction in case parameters are empty
if [ -z "$BENCH" ]
then
   echo "Benchmark name is empty";
   helpFunction
fi

if [ -z "$FS" ]
then
   echo "Mountfs is empty. Using none";
   FS=none
fi


if [ -z "$N_CORES" ]
then
   echo "Number of cores is empty. Using 56";
   N_CORES=56
fi

if [ -z "$EXP_FOLDER_NAME" ]
then
   echo "Result folder name is empty. Using DD_MM_YYYY";
   EXP_FOLDER_NAME="$(date '+%d_%m_%Y')"
   EXP_FOLDER_NAME="${EXP_FOLDER_NAME}_${BENCH}_${FS}_${N_CORES}"
fi

# Begin script in case all parameters are correct
echo "$BENCH"
echo "$FS"
echo "$EXP_FOLDER_NAME"
echo "$N_CORES"
### PARSE CMDLINE ARGS 

##########################

### VARS

[ ! -d ${EXP_FOLDER_NAME} ] && mkdir -p ${EXP_FOLDER_NAME}

##########################


# sudo mount -t tmpfs -o size=10G tmpfs /tmp/

if [[ ${FS} != "none" ]]; then
    sudo ./mkmounts "${FS}"
fi

TID=
PID=
PROCESS_NAME=
case $BENCH in 
    psearchy)
        PROCESS_NAME=pedsort
        ;;
    metis)
        PROCESS_NAME=wrmem
        ;;
    exim)
        PROCESS_NAME=exim-mod
        ;;
    *)
        echo "Unsupported benchmark"
        exit 1
        ;;
esac

echo "$PROCESS_NAME"

sudo ./config.py -d -c $N_CORES $BENCH >$EXP_FOLDER_NAME/run.log 2>&1 &

echo "Starting Profiling"
while [[ -z $TID ]]
do
    TID=`ps aux -T|grep $PROCESS_NAME | grep -v grep | awk '{print $3}'`
done

N_TID=`echo "$TID" | wc -l`

if [[ $N_CORES -gt 1 ]]
then
   while [ $N_TID -le 2 ]
   do
      TID=`ps aux -T|grep $PROCESS_NAME | grep -v grep | awk '{print $3}'`
      N_TID=`echo "$TID" | wc -l`
   done
fi
sleep 0.01


# This part is only necessary for getting a single TID
#============================================================
# echo "Getting TID"
# TID=`ps aux -T|grep $PROCESS_NAME | grep -v grep | awk '{print $3}'`
# N_TID=`echo "$TID" | wc -l`
# INDEX=`expr $N_TID / 2`
# echo "N_TID: $N_TID"
# echo "INDEX: $INDEX"
# TID=`echo "$TID" | head -n $INDEX | tail -n 1`
# echo "TID: $TID"
#============================================================

PID=`ps aux|grep $PROCESS_NAME | grep -v grep | awk '{print $2}' | tail -n 1`

echo "Launching Perf"
#sudo perf record -F 250 -s -g --call-graph dwarf -p $TID
#sudo /lib/linux-tools-5.4.0-132/perf record -s -p $TID
sudo /lib/linux-tools-5.4.0-132/perf record -ag --call-graph dwarf -s & 
#sudo /lib/linux-tools-5.4.0-132/perf record -ag -s -p $PID 
#sudo /lib/linux-tools-5.4.0-132/perf stat -e cycles:u,instructions:u,cycles:k,instructions:k -p $PID 

# This part is only necessary for the perf running in background
#============================================================
while [[ ! -z $TID ]]
do
   TID=`ps aux -T|grep $PROCESS_NAME | grep -v grep | awk '{print $3}'`
#   PID=`ps aux|grep $PROCESS_NAME | grep -v grep | awk '{print $2}'`
done

sudo pkill -SIGTERM perf


while [[ ! -f "perf.data" ]]
do
   echo "Wait for perf write"
   sleep 1
done

#============================================================

sudo mv perf.data $EXP_FOLDER_NAME/
echo "Perf end"
