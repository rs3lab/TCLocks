#!/usr/bin/python

import qmp
import sys
import errno
import os

cpuinfo = [dict(map(str.strip, line.split(":", 1))
                for line in block.splitlines())
           for block in file("/proc/cpuinfo", "r").read().split("\n\n")
           if len(block.strip())]

# Keep only primary hyperthreads
primaries = set()
for cpu in cpuinfo:
    processor = cpu["processor"]
    try:
        s = file("/sys/devices/system/cpu/cpu%s/topology/thread_siblings_list" % processor).read()
    except EnvironmentError, e:
        if e.errno == errno.ENOENT:
            primaries.add(processor)
            continue
        raise
    ss = set(map(int, s.split(",")))
    if int(processor) == min(ss):
        primaries.add(processor)
cpuinfo = [cpu for cpu in cpuinfo if cpu["processor"] in primaries]

def seq(cpuinfo):
    packages = {}
    package_ids = set()
    for cpu in cpuinfo:
        if "physical id" in cpu:
            package_id = int(cpu["physical id"])
            packages.setdefault(package_id, []).append(cpu)
            if cpu["processor"] is "0":
                cpu0_package_id = int(package_id)
            package_ids.add(int(package_id))
        else:
            yield cpu
    for cpu in packages[cpu0_package_id]:
        yield cpu
    package_ids.remove(cpu0_package_id)

    for package_id in package_ids:
        for cpu in packages[package_id]:
            yield cpu

def onlinecpu():
    import numa

    for node in range(0,numa.get_max_node()+1):
        for cpu in sorted(numa.node_to_cpus(node)):
            yield str(cpu)

def plseqorder():
    import numa
    o_cpus = []

    onlineCPUs = onlinecpu()
    cpus = [cpu for cpu in onlineCPUs]
    for cpu in seq(cpuinfo):
        cpus.remove(cpu["processor"])
        o_cpus.append(int(cpu["processor"]))
    for cpu in cpus:
        o_cpus.append(cpu)
    return o_cpus

def pin_proc(pid, core):
    import psutil

    try:
        print str(pid) + ":" + str(core)
	psutil.Process(pid).cpu_affinity([core])
    except ValueError as e:
	    print >> sys.stderr, e
	    sys.exit(1)


# 1 --> port number
# 2 --> number of vcpus
o_cpus = plseqorder()
print o_cpus
query = qmp.QMPQuery("localhost:%s" % (sys.argv[1]))
response = query.cmd("query-cpus")['return']

for i in range(int(sys.argv[2])):
    pin_proc(int(response[i]['thread_id']), o_cpus[i])

