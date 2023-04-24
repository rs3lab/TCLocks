from mparts.manager import Task
from mparts.host import HostInfo
from mparts.util import Progress, maybeMakedirs
from support import ResultsProvider, SourceFileProvider, SetCPUs, PrefetchDir, \
    FileSystem, SystemMonitor, PerfRecording

import os

__all__ = []

__all__.append("GmakeLoad")
class GmakeLoad(Task, ResultsProvider, SourceFileProvider):
    __info__ = ["host", "srcPath", "objPath", "*sysmonOut"]

    def __init__(self, host, trial, cores, srcPath, objPath,
            sysmon, stats, perfRecord, linuxSrc, resultPath,
	    perfBin, noCaching):
        Task.__init__(self, host = host, trial = trial)
        ResultsProvider.__init__(self, cores)
        self.host = host
        self.srcPath = srcPath
        self.objPath = objPath
        self.sysmon = sysmon
        self.stats = stats
        self.perfRecord = perfRecord
        self.linuxSrc = linuxSrc
        self.resultPath = resultPath
        self.perfBin = perfBin
	self.noCaching = noCaching

    def __cmd(self, target):
        print(["make", "-C", self.srcPath, "O=" + self.objPath, "-j", str(self.cores*2), target])
        return ["make", "-C", self.srcPath, "O=" + self.objPath,
                "-j", str(self.cores*2), target]

    def wait(self, m):
        import time
        logPath = self.host.getLogPath(self)

        with Progress("Preparing build"):
	    # clear the cache
	    if self.noCaching is True:
                import time
		self.host.sysctl("vm.drop_caches", "3")
                time.sleep(5)

            # import ipdb; ipdb.set_trace()
            # Clean
            self.host.r.run(self.__cmd("clean"), stdout = logPath)
            #print "Ran clean"

            # # Gen defconfig
            #self.host.r.run(self.__cmd("defconfig"))
            #print "Ran defconfig"

            # Build init/main.o first.  This gets most of the serial
            # early build stages out of the way.
            self.host.r.run(self.__cmd("init/main.o"), stdout = logPath)
            print "Ran init"

        # Build for real
        #
        # XXX If we want to eliminate the serial startup, monitor
        # starting with "  CHK include/generated/compile.h" or maybe
        # with the first "  CC" line.
	perfFile = os.path.join(os.path.abspath("."), self.resultPath,
			"perf-gmake-%s.dat" % self.cores)
	with PerfRecording(self.host, perfFile,
			self.perfRecord, self.perfBin):
	    self.host.r.run(self.sysmon.wrap(["taskset", "-c", "0-{}".format(self.cores-1)] + self.__cmd("vmlinux")),
                        stdout = logPath)
        # Get result
        log = self.host.r.readFile(logPath)
        self.sysmonOut = self.sysmon.parseLog(log)
        self.setResults(1, "build", "builds", self.sysmonOut["time.real"])

class GmakeRunner(object):
    def __str__(self):
        return "gmake"

    @staticmethod
    def run(m, cfg):
        host = cfg.primaryHost
        m += host
        m += HostInfo(host)
	fs = None
	if cfg.baseFSPath is "":
	    fs = FileSystem(host, cfg.fs, clean = True)
        else:
	    fs = FileSystem(host, cfg.fs,
			    basepath = cfg.baseFSPath, clean = True)
	print "fs value is: "+ str(fs)
        m += fs

        # XXX.
        # if cfg.kernelRoot doesn't exit
        #  - download & unarchive
        #  - from https://www.kernel.org/pub/linux/kernel/v3.x/linux-3.18.tar.xz

        # It's really hard to predict what make will access, so we
        # prefetch the whole source tree.  This, combined with the
        # pre-build of init/main.o, eliminates virtually all disk
        # reads.  For the rest, we'll just have to rely on multiple
        # trials or at least multiple configurations to cache.
	if cfg.noCaching is False:
	    m += PrefetchDir(host, cfg.kernelRoot, ["*/.git"])
        if cfg.hotplug:
            m += SetCPUs(host = host, num = cfg.cores)
        sysmon = SystemMonitor(host)
        m += sysmon
        for trial in range(cfg.trials):
            m += GmakeLoad(host, trial, cfg.cores, cfg.kernelRoot,
                    fs.path + "0", sysmon, cfg.pstat, cfg.precord,
                    cfg.linuxSrc, m.tasks()[0].getPath(), cfg.perfBin,
		    cfg.noCaching)
        # m += cfg.monitors
        m.run()

__all__.append("runner")
runner = GmakeRunner()
