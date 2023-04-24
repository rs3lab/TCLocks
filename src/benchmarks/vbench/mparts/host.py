import sys, os, hashlib, subprocess, tempfile

import rpc, server
from manager import Task, ResultPath
from util import maybeMakedirs, relpath, isLocalhost
from server import *

__all__ = ["Host", "SourceFileProvider", "HostInfo"] + server.__all__

# The main script's path is used to derive a unique scratch directory
# on each remote host.  This way we can more effectively reuse files
# between runs.
SCRIPT_PATH = os.path.realpath(sys.modules["__main__"].__file__)

class SourceFileProvider(object):
    """SourceFileProvider is a mixin class for Tasks that need to copy
    local files to a remote host.  When a host starts, it will scan
    the manager for any SourceFileProvider's and copy the queued
    source files to the remote host.  The local file hierarchy is
    reproduced on the remote side (with the local root directory
    mapped to a temporary directory), so relative relationships
    between files are kept intact."""

    def queueSrcFile(self, host, path):
        """Queue a file to be sent to the given host.  path should be
        relative to the module of the calling class.  Returns a
        relative path that may be used to refer to the file on the
        remote host once it has been transferred."""

        # Convert the module-relative path into an absolute path
        #
        # XXX This will fall apart if a superclass tries to use
        # queueSrcFile, since we'll see the module of the subclass.
        modBase = os.path.dirname(
            os.path.realpath(sys.modules[self.__module__].__file__))
        abspath = os.path.join(modBase, path)
        # Make sure it exists
        os.stat(abspath)
        # Add it to the queue
        self.__getQueue(host).append(abspath)
        return relpath(abspath)

    def getSourceFiles(self, host):
        """Return the list of source files queued for the given
        host."""
        return self.__getQueue(host)

    def __getQueue(self, host):
        # This is designed to work without an init call
        assert isinstance(host, Host)
        try:
            q = self.__queues
        except AttributeError:
            q = self.__queues = {}
        return q.setdefault(host, [])

class Host(Task, SourceFileProvider):
    """A Host provides access to a local or remote host and manages
    sending source files and retrieving result files.

    This provides an RPC stub for mparts.server.RemoteHost as the
    properties 'r' (regular user) and 'sudo' (root user), which have
    methods for remote command execution and file system access.  For
    details, see mparts.server.RemoteHost.

    This also manages sending source files to the remote host when
    the task is started (see SourceFileProvider) and retrieving result
    files when the task is stopped.  Other tasks should arrange for
    their remote output files to be placed under outDir() and this
    will take care of copying them to the local results directory.

    Finally, this provides a few miscellaneous services.  It
    manages rudimentary network routing information between hosts, in
    case special interfaces should be used to communicate over high
    bandwidth links between certain hosts.  It also provides a wrapper
    for controlling sysctl's."""

    __info__ = ["host"]

    def __init__(self, host, cmdModifier = None):
        """Create a Host task that will connect to the given host name
        via ssh.  host must be a routable host name for any of the
        hosts used in the experiment.  DO NOT use 'localhost'.  This
        will automatically detect if host is the local host and forgo
        ssh.

        cmdModifier, if given, is a function that can modify
        the ssh command; it will be passed the ssh part of the
        command, the sudo part of the command, and the remote server
        execution part of the command and should return the actual
        command to execute."""

        # Just a sanity check
        if host == "localhost" or host == "127.0.0.1":
            raise ValueError("Host name must be routable from all of the "
                             "hosts in the experiment.")

        Task.__init__(self, host = host)
        self.host = host
        self.__cmdModifier = cmdModifier
        self.__routes = {}
        self.__rConn = None
        self.__sudoConn = None
        self.__rootDir = "/tmp/mparts-%x" % abs(hash(SCRIPT_PATH))
        self.__isLocalhost = isLocalhost(host)

        for p in ["__init__.py", "server.py", "rpc.py"]:
            self.queueSrcFile(self, p)

    @property
    def r(self):
        """The mparts.server.RemoteHost stub for the regular user on
        this host."""

        if self.__rConn != None:
            return self.__rConn.r
        raise RuntimeError("Not connected to %s" % self)

    @property
    def sudo(self):
        """The mparts.server.RemoteHost stub for the root user on this
        host."""

        if self.__sudoConn != None:
            return self.__sudoConn.r

        # Make sure we've connected to the host
        self.r

        # Start our sudo connection.  We do this lazily because we may
        # not need root access on a host.
        self.__sudoConn = self.__connect(True)
        self.__sudoConn.r.init(self.__rootDir, os.getcwd(), "root@" + str(self))
        return self.__sudoConn.r

    def __str__(self):
        return self.host

    def toInfoValue(self):
        return str(self)

    def addRoute(self, target, hostname):
        """Add a special route to the Host target, indicating that
        processes running on this host should use the string
        'hostname' to connect to target."""

        assert isinstance(target, Host)
        self.__routes[target] = hostname

    def routeToHost(self, target):
        """Retrieve the string hostname that should be used to connect
        to the Host target from this host.  If no special route has
        been indicates, this will simply be the host name of the
        target host."""

        return self.__routes.get(target, target.host)

    def start(self, m):
        """Start this host.  Send source files and create the remote
        RPC server if necessary."""

        self.__sendSourceFiles(m)
        if self.__rConn == None:
            self.__rConn = self.__connect(False)
        # We init each time because rsync nukes both our remote cwd
        # and out directory.
        self.r.init(self.__rootDir, os.getcwd(), str(self))
        # Likewise, we have to re-init our sudo connection, if we have
        # one
        if self.__sudoConn:
            self.sudo.init(self.__rootDir, os.getcwd(), "root@" + str(self))

    def stop(self, m):
        """Stop this host.  Fetch result files into the local results
        directory as indicated by the ResultPath object in the
        manager.  This does *not* stop the remote RPC server, which is
        reused across experiments."""

        # Check that none of the output files clobber files we have
        ld = m.find1(cls = ResultPath).ensure()
        for path in self.r.listOutFiles():
            lpath = os.path.join(ld, path)
            if os.path.exists(lpath):
                raise ValueError("Duplicate output file %s" % path)
        # Fetch output files.  *Don't* use -a.  We don't want to
        # preserve most things (especially not permissions, since we
        # used funky permissions to let both the regular and root user
        # share the output directory).
        remPath = os.path.join(self.__rootDir, "out") + "/"
        cmd = ["rsync", "-rLts", "--out-format=copying %s: %%n%%L" % self.host,
               ("" if self.__isLocalhost else self.host + ":") + remPath,
               ld]
        subprocess.check_call(cmd)

    def reset(self):
        # Something went wrong.  Nuke my connection if I have one in
        # an effort to clean up.
        if self.__rConn:
            self.__rConn.close()
            self.__rConn = None
        if self.__sudoConn:
            self.__sudoConn.close()
            self.__sudoConn = None

    def __connect(self, sudo):
        # Figure out where we should start python to get the import
        # hierarchy right
        impRoot = os.path.join(self.__rootDir,
                               os.path.dirname(os.path.dirname(
                    os.path.realpath(sys.modules["mparts"].__file__))))
        remImpRoot = os.path.join(self.__rootDir, impRoot.lstrip("/"))

        # ssh to the remote host
        if self.__isLocalhost:
            cmdSsh = []
            cwd = remImpRoot
        else:
            cmdSsh = ["ssh", self.host,
                      "cd", remImpRoot, "&&"]
            cwd = None
        cmdSudo = ["sudo"] if sudo else []

        cmdRun = ["python", "-u", "-m", "mparts.server"]
        if self.__cmdModifier:
            cmd = self.__cmdModifier(self, cmdSsh, cmdSudo, cmdRun)
        else:
            cmd = cmdSsh + cmdSudo + cmdRun
        # Start the remote Python.  Put it into its own process group
        # so that Ctrl-C doesn't kill it directly and we can still use
        # our host connection while cleaning up.  It's too bad we
        # can't do this after we know it's up and running.
        server = subprocess.Popen(cmd, stdin = subprocess.PIPE,
                                  stdout = subprocess.PIPE, cwd = cwd,
                                  preexec_fn = lambda: os.setpgid(0,0))

        # Start up RPC client
        conn = rpc.RPCClient(server.stdin, server.stdout)

        return conn

    def outDir(self, *extra):
        """Fetch the remote results directory absolute path name.  For
        convenience, any arguments will be appended to the result path
        in the manner of os.path.join."""

        return os.path.join(self.__rootDir, "out", *extra)

    def getLogPath(self, task):
        """Get the remote log file name for the given task.  This is
        simply an output file, but follows the standard naming
        convention of 'log/<taskname>'."""

        # The entity name should be unique across hosts, so there's no
        # need to uniquify it
        return self.outDir("log", task.name)

    def __sendSourceFiles(self, m):
        # Gather source files
        sfs = []
        for obj in m.find(cls = SourceFileProvider):
            for path in obj.getSourceFiles(self):
                sfs.append(os.path.normpath(os.path.join(os.getcwd(), path)))

        # Rsync source files, nuking anything else in the host
        # directory (including old 'out' directories) while we're at
        # it.  Since we're cherry-picking paths and want to reproduce
        # our file hierarchy on the remote side, we use
        # include/exclude rules.
        #
        # XXX This runs without the perflock
        #
        # XXX It would be awesome if we could avoid deleting remote
        # .pyc and .o files, since this forces those to get rebuilt.
        cmd = ["rsync", "-aRs", "--out-format=%%oing %s: %%n%%L" % self.host,
               "--delete-excluded",
               "--filter", "P *.pyc",
               # XXX HACK HACK HACK.  Perhaps SourceFileProvider
               # should provide protect patterns.
               "--filter", "P *.o", "--filter", "P /home/amdragon/mosbench/memcached/mcload/mdc_udp"
               ]
        parents = set()
        for sf in sfs:
            # If it's a directory, we need to tell rsync to recurse
            if os.path.isdir(sf):
                sf = sf + "/**"
            # Include it
            cmd.append("--include=%s" % sf)
            # We also have to include all of its parent directories
            # (but with a trailing / so it doesn't copy their
            # contents).  Otherwise the final * exclude rule would cut
            # the parents and never get to the children.
            while sf != "/":
                sf = os.path.dirname(sf)
                p = sf if sf.endswith("/") else sf + "/"
                if p in parents:
                    break
                cmd.append("--include=%s" % p)
                parents.add(p)
        # Exclude anything we didn't include.
        cmd.extend(["--exclude=*", "/"])
        # Whew.
        if self.__isLocalhost:
            cmd.append(self.__rootDir)
        else:
            cmd.append(self.host + ":" + self.__rootDir)
        subprocess.check_call(cmd)

    #
    # Utilities
    #

    def sysctl(self, var, val, noError = False):
        """Set a sysctl variable on this host.  If noError is True,
        suppress any error output and don't raise an exception if
        sysctl fails.  Returns whether or not the sysctl succeeded."""

        if isinstance(val, bool):
            val = int(val)
        cmd = ["sysctl", "%s=%s" % (var, val)]
        if noError:
            s = self.sudo.run(cmd, wait = server.UNCHECKED,
                              stderr = server.DISCARD)
        else:
            s = self.sudo.run(cmd)
        return s.getCode() == 0

class HostInfo(Task):
    """HostInfo gathers various general semi-static information about
    a remote host's kernel, including the name of the kernel, the
    uname string, all sysctl settings, and the kernel configuration.
    sysctl settings are retrieved when stopping in case they were
    modified by any other tasks.  The kernel configuration is both
    reflected in the task configuration and is copied into the results
    directory."""

    __info__ = ["host", "uname", "kernel", "*sysctla", "*kconfig"]

    def __init__(self, host):
        Task.__init__(self, host = host)
        self.host = host
        self.uname = self.kernel = None
        self.sysctla = None

    def start(self):
        unamep = self.host.r.run(["uname", "-a"], stdout = server.CAPTURE)
        uname = unamep.stdoutRead()
        self.uname = uname.strip()
        self.kernel = uname.split()[2]

        # Get kernel configuration
        # Try /proc/config.gz first
        configPath = "/proc/config.gz"
        dstExt = ""
        try:
            config = self.host.r.readGzipFile(configPath)
            dstExt = ".gz"
        except EnvironmentError, e:
            configPath = "/boot/config-%s" % self.kernel
            try:
                config = self.host.r.readFile(configPath)
            except EnvironmentError, e:
                self.log("Failed to fetch kernel config: %s" % e)
                return

        self.kconfig = self.__parseConfig(config)
        self.host.r.run(["cp", configPath,
                         os.path.join(self.host.outDir(),
                                      self.name + ".kconfig" + dstExt)])
        self.host.r.writeFile(os.path.join(self.host.outDir(), self.name + ".uname"), uname)

    def __parseConfig(self, config):
        res = {}
        for line in config.splitlines():
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split("=")
            if len(parts) != 2:
                raise ValueError("Failed to parse kernel config line %r" % line)
            if parts[1].isdigit():
                # We could parse a lot more, ranging from hex numbers
                # to strings.  I intentionally avoid anything that
                # wouldn't print the way it appears in the config.
                parts[1] = int(parts[1])
            res[parts[0]] = parts[1]
        return res

    def stop(self):
        # We gather sysctl settings here because other tasks may have
        # changed them
        self.sysctla = {}
        sc = self.host.r.run(["/sbin/sysctl", "-a"],
                             stdout = server.CAPTURE,
                             stderr = server.DISCARD).stdoutRead()
        for l in sc.splitlines():
            k, v = l.split(" = ", 1)
            if v.isdigit():
                # Most are ints.  The rest we just leave as strings,
                # though we could split arrays.
                v = int(v)
            self.sysctla[k] = v
