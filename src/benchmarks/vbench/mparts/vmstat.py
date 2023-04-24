#!/usr/bin/env python

import time
import signal
import itertools
import argparse
import operator
import sys
import subprocess
from multiprocessing import Process
import os

def now():
    return time.time()

file_handler = None
end = False

class Source:
    """
    Uniquely identifies place at which measurable value was obtained.

    """
    def __init__(self, path=[]):
        self.path = tuple(path)

    def __repr__(self):
        return '/'.join(self.path)

    def __eq__(self, other):
        return self.path == other.path

    def __hash__(self):
        return hash(self.path)

    def __nonzero__(self):
        return bool(self.path)


class Measurable:
    """
    Descruibes a class of things which can be measured.
    Associated with Source and value creates a Sample.

    """
    def __init__(self, name, description=None, tags=[]):
        self.name = name
        self.description = description
        self.tags = set(tags)

    def addTag(self, tag):
        self.tags.add(tag)

    def __str__(self):
        return self.name

    def __repr__(self):
        return self.name


class Counter(Measurable):
    """
    Measurable which changes monotonically

    """
    pass


class Subject:
    def __init__(self, source, measurable):
        self.source = source
        self.measurable = measurable

    def __repr__(self):
        return str("%s/%s" % (self.source, self.measurable))

    def __eq__(self, other):
        return self.source == other.source and self.measurable == other.measurable

    def __hash__(self):
        return hash((self.source, self.measurable))

class Sample:
    def __init__(self, subject, time, value):
        self.subject = subject
        self.time = time
        self.value = value

    def split_by_subject(self):
        return (self.subject, (self.time, self.value))


def tagWith(tag_name, *args):
    for measurable in args:
        measurable.addTag(tag_name)
    return args

KVMCounters = []
KVMCounters.extend(tagWith("kvm_exits",
    Counter("exits",               "VM exits"),
    Counter("irq_exits",           "VM exits (external interrupt)"),
    Counter("irq_window",          "VM exits (interrupt window"),
    Counter("request_irq",         "VM exits (interrupt window request)"),
    Counter("io_exits",            "VM exits (PIO)"),
    Counter("mmio_exits",          "VM exits (MMIO)"),
    Counter("signal_exits",        "VM exits (host signal)"),
    Counter("halt_exits",          "VM exits (halt)"),
    Counter("halt_wakeup",         "Halt wakeups"),
    Counter("halt_successful_poll","Halt successful poll")))

KVMCounters.extend(tagWith("kvm_intr",
    Counter("irq_injections",      "IRQ injections"),
    Counter("nmi_injections",      "NMI injections"),
    Counter("nmi_window",          "NMI window")))

KVMCounters.extend(tagWith("kvm_mmu",
    Counter("mmu_cache_miss",      "MMU cache misses"),
    Counter("mmu_flooded",         "MMU floods"),
    Counter("mmu_pde_zapped",      "MMU PDE zaps"),
    Counter("mmu_pte_updated",     "MMU PTE updates"),
    Counter("mmu_pte_write",       "MMU PTE writes"),
    Counter("mmu_recycled",        "MMU recycles"),
    Counter("mmu_shadow_zapped",   "MMU shadow zaps"),
    Counter("mmu_unsync",          "MMU unsyncs")))

KVMCounters.extend(tagWith("kvm_tlb",
    Counter("remote_tlb_flush",    "TLB flushes (remote)"),
    Counter("invlpg",              "TLB entry invalidations (INVLPG)"),
    Counter("tlb_flush",           "TLB flushes")))

KVMCounters.extend(tagWith("kvm_paging",
    Counter("largepages",          "Large pages in use"),
    Counter("pf_fixed",            "Fixed (non-paging) PTEs"),
    Counter("pf_guest",            "Page faults injected")))

KVMCounters.extend(tagWith("kvm_reload",
    Counter("host_state_reload",   "Host state reloads"),
    Counter("efer_reload",         "EFER reloads"),
    Counter("fpu_reload",          "FPU reloads"),
    Counter("hypercalls",          "Hypervisor service calls")))
    #Counter("hlt_hypercalls",      "Hypervisor service calls for halt"),
    #Counter("irq_hypercalls",       "Hypervisor service calls for irq")))

KVMCounters.extend(tagWith("kvm_emul",
    Counter("insn_emulation",      "Emulated instructions"),
    Counter("insn_emulation_fail", "Emulated instructions (failed)")))

def probeKVMCounters():
    source = Source(['kvm'])
    for counter in KVMCounters:
        with open('/sys/kernel/debug/kvm/%s' % (counter.name), 'r') as f:
            yield Sample(Subject(source, counter), now(), int(f.read()))

def probeAll():
    return list(itertools.chain(
        probeKVMCounters()))

def hashSubject(samples):
    return dict(map(Sample.split_by_subject, samples))

def delta(prev_samples, new_samples):
    old_values = hashSubject(prev_samples)
    new_values = hashSubject(new_samples)
    diff = {}
    for key, old_measurements in old_values.iteritems():
        diff[key] = map(operator.sub, new_values[key], old_measurements)
    return diff

def printDiffCSV(counter, prev_samples, new_samples):
    delta_probes = delta(prev_samples, new_samples)
    keys = []
    values = []
    for key in sorted(delta_probes.keys(), key=str):
        dt, dval = delta_probes[key]

        keys.append(str(key))
        values.append(str(dval))

    if counter == 0:
        file_handler.write(",".join(keys))
        file_handler.write("\n")

    file_handler.write(",".join(values))
    file_handler.write("\n")
    file_handler.flush()

def signal_handler(signal, frame):
    end = True
    file_handler.close()
    sys.exit(0)

def monitorKVMNumbers(filepath):
    import signal
    signal.signal(signal.SIGINT, signal_handler)
    global file_handler
    file_handler = open(filepath, "w")
    prev_samples = first_samples = probeAll()
    counter = 0
    while not end:
        time.sleep(1)
        new_samples = probeAll()
        printDiffCSV(counter, prev_samples, new_samples)
        prev_samples = new_samples
        counter = counter + 1
    file_handler.close()

def monitorPerf(src):
    print "Try perf"
    try:
        args = 'sudo %s/tools/perf/perf kvm --host --guest ' \
            '--guestkallsyms=guest.kallsyms --guestvmlinux=%s/vmlinux ' \
            '--guestmodules=guest.modules record -a' % (src, src)
        print args
	subprocess.check_call(args, stdout=None,
			stderr=None, shell=True)
    except subprocess.CalledProcessError:
	print ""

def monitorHostPerf(src):
    print "Try Host perf"
    try:
        args = 'sudo %s/tools/perf/perf record -a ' \
            '-g -o %s' % (src[0], src[1])
        print args
	subprocess.check_call(args, stdout=None,
			stderr=None, shell=True)
    except subprocess.CalledProcessError:
	print ""

def monitorGuestPerf(cores):
    print "Try Guest perf"
    try:
        args = 'ssh -p 5555 root@localhost ' \
            '/root/run-perf.sh %s' % (cores)
        print args
	subprocess.check_call(args, stdout=None,
			stderr=None, shell=True)
    except subprocess.CalledProcessError:
	 print ""

def dump_lock_stats(target):
    try:
	args = "sudo cat /proc/lock_stat > %s" % (target)
	print args
	subprocess.check_call(args, stdout=None,
			stderr=None, shell=True)
    except subprocess.CalledProcessError:
	print ""

function_mappings = {
    'perf': monitorPerf,
    'hostperf': monitorHostPerf,
    'guestperf': monitorGuestPerf,
    'KVMNumbers': monitorKVMNumbers,
    'lockstats': dump_lock_stats,
}

def StartMonitor(func, arg):
    proc = Process(target=function_mappings[func], args=(arg,))
    proc.start()
    return proc

def TerminateMonitor(proc):
    if proc.is_alive() is True:
        proc.terminate()
    else:
        print "process already died"

def TerminatePerf():
    args = "sudo kill -INT $(pgrep perf)"
    try:
	subprocess.check_call(args, stdout=None,
			stderr=None, shell=True)
    except subprocess.CalledProcessError:
	print ""

def compileKernel(sshPort, cores, srcPath, objectPath):
    args = "ssh -p %s root@localhost 'make -C %s O=%s -j %s vmlinux > /dev/null'" % \
	(sshPort, srcPath, objectPath, cores * 2)
    print args
    try:
	subprocess.check_call(args, stdout=None, stderr=None, shell=True)
    except subprocess.CalledProcessError:
	    print "compilation didn't work"


def StartLKC(sshPort, cores, srcPath, objectPath):
    proc = Process(target=compileKernel, args=(sshPort, cores, srcPath, objectPath))
    proc.start()

