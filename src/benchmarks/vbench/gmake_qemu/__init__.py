from mparts.manager import Task
from mparts.host import HostInfo
from mparts.util import Progress, maybeMakedirs, deleteFile
from support import ResultsProvider, SourceFileProvider, SetCPUs, PrefetchDir, \
    FileSystem, SystemMonitor, QemuVMDeamon, LockStats, PerfRecording, KVMTrace

import os
import mparts.vmstat

# qemu stuff required for booting
SSHPORT = 5555
QMPPORT = 4444
VNCPORT = 1

__all__ = []

__all__.append("GmakeQemuLoad")
class GmakeQemuLoad(Task, ResultsProvider, SourceFileProvider):
	__info__ = ["host", "srcPath", "objPath", "*sysmonOut"]

	def __init__(self, host, trial, cores, srcPath, objPath, sysmon,
                benchRoot, coresPerSocket, resultPath, linuxSrc, qpin,
		lockStats, multiVM, record, perfBin, perfKVMRec,
		perfGuestRec, kvmStat, kvmTrace):
		Task.__init__(self, host = host, trial = trial)
		ResultsProvider.__init__(self, cores)
		self.host = host
		self.srcPath = srcPath
		self.objPath = objPath
		self.sysmon = sysmon
                self.benchRoot = benchRoot
		self.coresPerSocket = coresPerSocket
                self.resultPath = resultPath
                self.perfRecord = record
		self.perfBin = perfBin
		self.perfKVMRec = perfKVMRec
		self.perfGuestRec = perfGuestRec
                self.linuxSrc = linuxSrc
                self.qpin = qpin
                self.multiVM = multiVM
                self.vmProcs = []
                self.sockets = 1
                self.vmcores = self.coresPerSocket
                if self.cores > 1:
                    self.sockets = self.cores / self.coresPerSocket
                else:
                    self.vmcores = 1
                self.threads = 1
                self.lockStats = LockStats(self.host, sshPort = "5555") if \
                        lockStats else None # only one VM is being considered
                self.kvmStat = kvmStat
		self.kvmTrace = kvmTrace


	def __cmd(self, target, sshPort="5555"):
		rlist = ["ssh", "root@localhost", "-p", sshPort,
		        'make -C %s O=%s -j %s %s' \
		        % (self.srcPath, self.objPath,
                            str(self.cores*2), target)]
		return rlist

	def wait(self, m):
		logPath = self.host.getLogPath(self)
                perfproc = None
                guestperfproc = None

		with Progress("Starting vm"):
                    import time

                    # Boot vm
                    # TODO: Fix the number generation for qmp and ssh port for VMs
                    absBenchRoot = os.path.abspath(".")
                    vmProc = QemuVMDeamon(self.host, absBenchRoot,
                            "ubuntu-server.img", self.vmcores, self.threads,
                            self.sockets, 4444, 5555, 1)
                    vmProc.startVM()
                    self.vmProcs.append(vmProc)
                    # checking for oversubscribed cloud case
                    if self.multiVM is True:
                        vmProcNew = QemuVMDeamon(self.host, absBenchRoot,
                                "ubuntu-server-copy.img", self.vmcores,
                                self.threads, self.sockets, 4445,
                                5556, 2)
                        vmProcNew.startVM()
                        self.vmProcs.append(vmProcNew)
                    print "wait for VM to come up, sleeping 46 seconds"
                    # Lets sleep for sometime as the VM booting takes some time
                    self.host.r.run(['sleep', '1'])
                    time.sleep(1)
                    if self.qpin is True:
			for proc in self.vmProcs:
				proc.pinVCPUs()
		    time.sleep(45)


		with Progress("Preparing build"):
                        # mount tmpfs-separate
                        def prepare(sshPort="5555"):
                            self.host.r.run(["ssh", "-p", sshPort, "root@localhost",
                                'sudo ./mkmounts  tmpfs-separate'])
                            # prefetch the directories
                            self.host.r.run(["ssh", "-p", sshPort, "root@localhost",
                                './prefetch  -r -x "*/.git" bench/fs-bench/tmp/linux-3.18/'])

                            # # Gen defconfig
                            self.host.r.run(self.__cmd("defconfig", sshPort))

                            # Build init/main.o first. This gets most of the serial
                            # early build stages out of th way.
                            self.host.r.run(self.__cmd("init/main.o", sshPort), stdout = logPath)

                        prepare();
                        if self.multiVM is True:
                            prepare("5556")

		# Build for real
		#
		# XXX If we want to eliminate the serial startup, monitor
		# starting with "  CHK include/generated/compile.h" or maybe
		# with the first "  CC" line.
		#self.host.sudo.run()
                self.host.sudo.run(['mount', '-o', 'remount,mode=0755',
                    '-t', 'debugfs', 'nodev', '/sys/kernel/debug'])

                # get the kallsyms and kmodules of guest

                proc = mparts.vmstat.StartMonitor("KVMNumbers", "data.csv")

                if self.lockStats is not None:
                    self.lockStats.allowCollectLockStatsHost(0)
                    self.lockStats.cleanLockStats()
                    self.lockStats.allowCollectLockStatsHost(1)

                if self.multiVM is True:
                    mparts.vmstat.StartLKC("5556", self.cores, self.srcPath, self.objPath)

		perfFile = os.path.join(os.path.abspath("."), self.resultPath,
				"perf-gmakeqemu-%s.dat" % self.cores)
		traceFile = os.path.join(os.path.abspath("."), self.resultPath,
				"kvmtrace-gmake-%s.dat" % self.cores)
		with PerfRecording(self.host, perfFile, self.perfRecord,
			self.perfBin, self.perfKVMRec, self.perfGuestRec):
			with KVMTrace(self.host, traceFile, self.kvmTrace):
				self.host.r.run(self.sysmon.wrap(self.__cmd("vmlinux")),
						stdout = logPath)
                if self.lockStats is not None:
                    self.lockStats.dumpLockStats(os.path.join(self.resultPath,
                        "lock-stats-%s" % (self.cores)))
                    self.lockStats.allowCollectLockStatsHost(0)

                mparts.vmstat.TerminateMonitor(proc)

		# Get result
		log = self.host.r.readFile(logPath)
		self.sysmonOut = self.sysmon.parseLog(log)
		self.setResults(1, "build", "builds", self.sysmonOut["time.real"])
                maybeMakedirs(self.resultPath)
                os.rename("data.csv", os.path.join(self.resultPath, "data.csv"))

		with Progress("Stopping vm"):
                    for proc in self.vmProcs:
                         proc.stopVM()


class GmakeQemuRunner(object):
	def __str__(self):
		return "gmakeqemu"

	@staticmethod
	def run(m, cfg):
		host = cfg.primaryHost
		m += host
		m += HostInfo(host)
		fs = FileSystem(host, cfg.fs, clean = True)
		m += fs
		if cfg.hotplug:
			m += SetCPUs(host = host, num = cfg.cores)
		sysmon = SystemMonitor(host)
		m += sysmon
		for trial in range(cfg.trials):
			m += GmakeQemuLoad(host, trial, cfg.cores,
                                cfg.kernelRoot, fs.path + "0", sysmon,
                                cfg.benchRoot, cfg.coresPerSocket,
                                m.tasks()[0].getPath(), cfg.linuxSrc,
				cfg.qpin, cfg.lockStats, cfg.multiVM,
				cfg.precord, cfg.perfBin, cfg.perfKVMRec,
				cfg.perfGuestRec, cfg.kvmStat, cfg.kvmTrace)
			# m += cfg.monitors
		m.run()

__all__.append("runner")
runner = GmakeQemuRunner()
