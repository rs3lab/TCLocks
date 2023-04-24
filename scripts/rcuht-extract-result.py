#!/usr/bin/python3

import sys
import os
import os.path
import re
import optparse

lock = str(os.environ['LOCK'])
lock_type = str(os.environ['LOCK_TYPE'])
cores = str(os.environ['CORES'])

if lock_type == "spinlock":
    bench = ["table_spinlock","table_aqs","table_cna","table_komb"]
elif lock_type == "spinlock-prefetch":
    bench = []
    for i in range(2,9,2):
        bench.append("table_komb-"+str(i)+"prefetchlines")
elif lock_type == "spinlock-batch-size":
    bench = []
    cnt=1024
    for i in range(3,7):
        bench.append("table_komb-"+str(cnt)+"batchsize")
        cnt = cnt * 4
elif lock_type == "spinlock-optimization":
    bench = ["table_spinlock", "table_komb-baseline","table_komb-numa","table_komb-numaprf","table_komb-numaprfwwjump"] 
elif lock_type == "mutex":
    bench = ["table_mutex","table_komb_mutex"] 
elif lock_type == "rwsem":
    bench = ["table_rwsem","table_rwaqm","table_komb_rwsem"] 
else:
       raise Exception("Unknown lock_type") 

runs = [1]
if lock_type == "rwsem":
    rw_writes = [1, 20]
else
    rw_writes = [100]
time = 30
buckets = 1024
entries = 4096
parent_dir="../doc/results/"
result_folder=parent_dir+"results-"+lock+"-"+str(cores)+"cores-"+str(time)+"seconds/"+str(buckets)+"buckets-"+str(entries)+"entries/"

if lock_type == "mutex":
    core = [1,2,4,8,12,16,20,28,56,84,112,128,168,224,336,448,576,672,800,896]
else:
    core = [1,2,4,8,12,16,20,28,56,84,112,128,168,224]

plot_file_name=result_folder+"rcuht-"+lock_type+"-"+str(cores)+"cores-"+str(time)+"seconds.pdf"

if __name__ == "__main__":
	for writes in rw_writes:
		out_file_name= result_folder+lock_type+"-"+str(writes)+"-percent-writes.csv"
		out_file= open(out_file_name,"w")
		index=0
		plot_file.write("plot ")
		for b in bench:
			out_file.write("# "+ b+"\n")
			out_file.write("# Cores, Throughput (Jobs/us)\n")
			index = index + 1
			for c in core:
				total_ops = 0
				total_runs = 0
				for r in runs:
					path = result_folder +"/"+b + "/"+str(writes)+"percent_writes/core." + str(c)
					if os.path.isfile(path):
						with open(path, "r") as f:
							for line in f:
								if "summary: total:" not in line:
									continue
								line = line.split()
								ops = float(line[3])*1000/float(line[5])
								total_ops += ops
								total_runs += 1
				if total_runs != 0:
					out_file.write(str(c)+","+str(total_ops/total_runs)+"\n")
			out_file.write("\n\n")
