#!/usr/bin/python

import sys
import os
import os.path
import re

bench = ["orig_qspinlock", "aqs", "dsmsynch"] 
#core = [1, 14, 28, 42, 56, 70, 84, 98, 112, 126, 140, 154, 168, 182, 196, 210, 224]
core = [1, 14,28 ,42, 56, 84, 112, 140, 168, 196, 224]
runs = [1, 2, 3, 4, 5]
dirty_cls = [1, 4, 8, 16]
cores = 224
time = 30
result_folder="../results-sync-stresser-spinlock-"+str(cores)+"cores-"+str(time)+"seconds/"

if __name__ == "__main__":

	for dcl in dirty_cls:
		out_file= open("sync-stresser-spinlock-"+str(dcl)+"dirty_cl.csv","w")
		for b in bench:
			out_file.write("# "+ b+"\n")
			out_file.write("# Cores, Throughput (Jobs/us)\n")
			for c in core:
				total_ops = 0
				total_runs = 0
				for r in runs:
					path = result_folder +str(r)+"/"+b + "/"+str(dcl)+"_dirty_cl/core." + str(c)
					if os.path.isfile(path):
						with open(path, "r") as f:
							for line in f:
								if "summary: jobs:" not in line:
									continue
								line = line.split()
								ops = float(line[2])*1000/float(line[5])
								total_ops += ops
								total_runs += 1
				out_file.write(str(c)+","+str(total_ops/total_runs)+"\n")
			out_file.write("\n\n")
