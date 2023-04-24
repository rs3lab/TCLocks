#!/usr/bin/env python3

from qmp import QEMUMonitorProtocol
import sys

def onlinecpu():
    import numa

    o_cpus = []
    for node in range(0,numa.get_max_node()+1):
        for cpu in sorted(numa.node_to_cpus(node)):
            o_cpus.append(cpu)

    return o_cpus


def pin_proc(pid, core):
    import psutil

    try:
        psutil.Process(pid).cpu_affinity([core])
    except ValueError as e:
        print >> sys.stderr, e
        sys.exit(1)


# 1 --> port number
# 2 --> number of vcpus

if len(sys.argv) < 2:
    port_number=5555
    num_cores=224
else:
    port_number=int(sys.argv[1])
    num_cores=int(sys.argv[2])

query = QEMUMonitorProtocol(("127.0.0.1",port_number))
query.connect()
response = query.cmd("query-cpus-fast")["return"]
o_cpus = [x for x in range(224)]

for i in range(int(num_cores)):
    print(int(response[i]['thread-id']), o_cpus[i])
    pin_proc(int(response[i]['thread-id']), int(o_cpus[i]))

