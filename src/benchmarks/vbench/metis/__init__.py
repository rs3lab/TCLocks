from mparts.manager import Task
from mparts.host import HostInfo, CHECKED
from support import ResultsProvider, SetCPUs, FileSystem, SystemMonitor

import os

__all__ = []

__all__.append("MetisLoad")
class MetisLoad(Task, ResultsProvider):
    __info__ = ["host", "trial", "metisPath", "streamflow", "model",
                "*sysmonOut"]

    def __init__(self, host, trial, cores, metisPath, streamflow, model,
                 setcpus, fs, sysmon):
        assert model in ["default", "hugetlb"], \
            "Unknown Metis memory model %r" % model

        Task.__init__(self, host = host, trial = trial)
        ResultsProvider.__init__(self, cores)
        self.host = host
        self.trial = trial
        self.metisPath = metisPath
        self.streamflow = streamflow
        self.model = model
        self.setcpus = setcpus
        self.fs = fs
        self.sysmon = sysmon

    def wait(self, m):
        # Clean the file system.  Metis assumes memory it allocates
        # from hugetlbfs will be zeroed, so we can't have stale page
        # files sitting around.
        if self.fs:
            self.fs.clean()

        obj = os.path.join(self.metisPath, "obj")
        cmd = [os.path.join(os.path.join(obj, "app", "wrmem" + (".sf" if self.streamflow else ""))),
        #cmd = [os.path.join(os.path.join(obj, "app", "hist" + (".sf" if self.streamflow else ""))), os.path.join(self.metisPath, "data", "hist-5.2g.bmp"),
               "-p", str(self.cores)]
        # cmd = [os.path.join(self.metisPath, "test/test_wrmem.pl")]
        # cmd = self.sysmon.wrap(cmd, "Starting mapreduce", "Finished mapreduce")
        cmd = self.sysmon.wrap(cmd)
        objhugetlb = os.path.join(self.metisPath, "obj") + "." + self.model
        addEnv = {"LD_LIBRARY_PATH" : os.path.join(objhugetlb, "lib"),
                  "CPUSEQ" : self.setcpus.getSeqStr()}

        cmd = ["taskset", "-c", "0-{}".format(self.cores-1)] + cmd
        print(cmd)

        # Run
        logPath = self.host.getLogPath(self)
        self.host.r.run(cmd, stdout = logPath, addEnv=addEnv, wait = CHECKED)

        # Get result
        log = self.host.r.readFile(logPath)
        self.sysmonOut = self.sysmon.parseLog(log)
        self.setResults(1, "job", "jobs", self.sysmonOut["time.real"])

class MetisRunner(object):
    def __str__(self):
        return "metis"

    @staticmethod
    def run(m, cfg):
        # XXX Clean hugetlb directories between trials?
        host = cfg.primaryHost
        m += host
        m += HostInfo(host)
        if cfg.model == "hugetlb":
            # We explicitly clean before each trial
            fs = FileSystem(host, "hugetlb", clean = False)
            m += fs
        else:
            fs = None
        metisPath = os.path.join(cfg.benchRoot, "metis")
        setcpus = SetCPUs(host = host, num = cfg.cores, hotplug = cfg.hotplug,
                          seq = cfg.order)
        m += setcpus
        sysmon = SystemMonitor(host)
        m += sysmon
        for trial in range(cfg.trials):
            m += MetisLoad(host, trial, cfg.cores, metisPath, cfg.streamflow,
                           cfg.model, setcpus, fs, sysmon)
        # m += cfg.monitors
        m.run()

__all__.append("runner")
runner = MetisRunner()
