#!/bin/bash

# login as root, and run tools/mount.sh to mount hugetlbfs with
# sufficient memory.

if [ "$#" -lt "1" ]; then
  echo "Usage: $0 [min number of cores] [max numbeer of cores]"
  exit;
fi
ncores=`grep -c processor /proc/cpuinfo`
user=`id -u --name`
if [ "$#" -eq "2" ]; then
  max=$2
else
  max=$ncores
fi
echo cores: $1-$max
sudo chown $user /mnt/huge -R

for ((i=$1; i<=max; i++))
do
  rm /mnt/huge/* 2>/dev/null
  #./tools/perf record -g obj/app/wrmem.sf -p $i
  echo $i >> results/breakdown
  sudo /usr/bin/time -f "user\t%U\tsys\t%S\treal\t%E" obj/app/wrmem.sf -p $i >> results/profile 2>>results/breakdown
   #sudo obj/app/wrmem.sf -p $i
done

# login as root, and run tools/umount.sh to umount hugetlbfs
