from __future__ import with_statement

import sys, os, subprocess, errno, hashlib, threading, signal, gzip
from mparts.rpc import RPCServer, RPCProxy

LOG_COMMANDS = False

__all__ = ["CAPTURE", "STDERR", "DISCARD",
           "CHECKED", "UNCHECKED"]

CAPTURE = "\0CAPTURE"
STDERR = "\0STDERR"
DISCARD = "\0DISCARD"

CHECKED = "CHECKED"
UNCHECKED = "UNCHECKED"

popenLock = threading.Lock()

class Process(object):
    def __init__(self, cmd, p, dw):
        self.__cmd = cmd
        self.__p = p
        self.__dw = dw
        if p.stdout:
            self.__stdout = []
            self.__stdoutCond = threading.Condition()
            self.__stdoutClosed = False
            t = threading.Thread(target = self.__reader)
            t.setDaemon(True)
            t.start()
        else:
            self.__stdout = None
        self.__waitLock = threading.Lock()

    def __reader(self):
        fd = self.__p.stdout.fileno()
        while True:
            buf = os.read(fd, 65536)
            with self.__stdoutCond:
                if len(buf) == 0:
                    self.__stdoutClosed = True
                    self.__p.stdout.close()
                else:
                    self.__stdout.append(buf)
                self.__stdoutCond.notifyAll()
            if len(buf) == 0:
                break

    def stdinWrite(self, s):
        self.__p.stdin.write(s)

    def stdinClose(self):
        self.__p.stdin.close()

    def __stdoutRead(self, fn, pred = None):
        if self.__stdout == None:
            raise ValueError("stdout of %s is not being captured" % self.__cmd)
        s = ""
        while True:
            with self.__stdoutCond:
                while len(self.__stdout) == 0:
                    if self.__stdoutClosed:
                        return s
                    self.__stdoutCond.wait()
                s += "".join(self.__stdout)
                self.__stdout[:] = []
                if pred == None or pred(s):
                    read, unread = fn(s)
                    if unread != "":
                        self.__stdout.append(unread)
                    return read

    def stdoutRead(self):
        return self.__stdoutRead(lambda s: (s, ""))

    def stdoutReadline(self):
        def split(s):
            nl = s.index("\n")
            return s[:nl+1], s[nl+1:]
        return self.__stdoutRead(split, lambda s: "\n" in s)

    def getCode(self):
        return self.__p.returncode

    def kill(self, sig = signal.SIGINT):
        os.kill(self.__p.pid, sig)

    def wait(self, check=True, poll=False):
        """Wait for this process to exit, returning its exit code (or
        -N if it died by signal N).  Unlike UNIX wait, this wait is
        idempotent.  If check is True, raise a ValueError if the
        return code is non-zero.  If poll is True, don't block."""

        # We serialize through a lock because the underlying wait call
        # will only return successfully to one concurrent wait and we
        # want to support multiple waits.  In practice, this occurs
        # when we're waiting on something, then the client gets a
        # KeyboardInterrupt and tells us to wait again in another
        # request (and thus thread) as part of shutting down.
        with self.__waitLock:
            if self.__p.returncode != None:
                code = self.__p.returncode
            elif poll:
                code = self.__p.poll()
            else:
                code = self.__p.wait()
                if self.__p.stdin:
                    self.__p.stdin.close()
                if self.__dw:
                    # XXX It's possible I could just dup the pipe FD
                    # and have only one death-pipe write FD, but I
                    # don't know what the signal ownership semantics
                    # are.
                    os.close(self.__dw)
                    self.__dw = None
        if check and code:
            if code < 0:
                msg = "signal %d" % (-code)
            else:
                msg = "status %d" % code
            raise ValueError("%s exited with %s" % (self.__cmd, msg))
        return code

SHELL_SPECIALS = set("\\'\"`<>|; \t\n()[]?#$^&*=")
def shellEscape(s):
    if not set(s).intersection(SHELL_SPECIALS):
        # No quoting necessary
        return s
    if not "'" in s:
        # Single quoting works
        return "'" + s + "'"
    # Have to double quote.  See man bash QUOTING.
    s = (s.replace("\\", "\\\\").replace("\"", "\\\"").replace("$", "\\$")
         .replace("`", "\\`").replace("!", "\\!"))
    return '"' + s + '"'

class RemoteHost(object):
    def init(self, rootDir, cwd, name):
        # XXX This is terrible.  Since we're sharing our root tree
        # between a regular user and root and most file operations,
        # including cleaning up the tree, are done as the regular
        # user, the root user has to permit this.  This is obviously
        # not the right answer.
        if os.getuid() == 0:
            os.umask(0)

        self.__rootDir = rootDir
        lcwd = self.__safePath(os.path.join(rootDir, cwd.lstrip("/")))
        self.__makedirs(lcwd)
        os.chdir(lcwd)

        self.__makedirs(os.path.join(rootDir, "out"))

        self.__name = name

    def __safePath(self, p):
        if not os.path.normpath(p).startswith(self.__rootDir):
            raise ValueError("The path %r is not in the remote root %r" %
                             (p, self.__rootDir))
        return p

    def __makedirs(self, path):
        try:
            os.makedirs(path)
        except EnvironmentError, e:
            if e.errno != errno.EEXIST:
                raise

    def listOutFiles(self):
        res = []
        base = os.path.join(self.__rootDir, "out")
        for dirpath, dirnames, filenames in os.walk(base):
            for n in filenames:
                abspath = os.path.join(dirpath, n)
                relpath = abspath[len(base):].lstrip("/")
                res.append(relpath)
        return res

    def __toOutFile(self, desc, noCheck = False):
        if desc == CAPTURE:
            return subprocess.PIPE
        elif desc == DISCARD:
            return file("/dev/null", "w")
        else:
            desc = os.path.expanduser(desc)
            if not noCheck:
                desc = self.__safePath(desc)
            self.__makedirs(os.path.dirname(desc))
            return file(desc, "a")

    def run(self, cmd, stdin = DISCARD, stdout = DISCARD, stderr = STDERR,
            cwd = None, shell = False, addEnv = {},
            wait = CHECKED, exitSig = signal.SIGINT, noCheck = False):
        # Set up stdin/stdout/stderr
        assert stderr != CAPTURE, "stderr capture not implemented"
        if stdin == DISCARD:
            pstdin = file("/dev/null")
        elif stdin == CAPTURE:
            pstdin = subprocess.PIPE
        elif stdin == STDERR:
            raise ValueError("Illegal stdin %s" % stdin)
        else:
            pstdin = file(os.path.expanduser(stdin), "r")

        if stdout == stderr == STDERR:
            # subprocess is really finicky.  If you pass the same
            # FD for both stdout and stderr, it will get closed.
            pstdout = os.dup(2)
        else:
            pstdout = self.__toOutFile(stdout, noCheck)

        if stderr == STDERR:
            pstderr = None
        else:
            pstderr = self.__toOutFile(stderr, noCheck)

        # Expand user
        cmd = map(os.path.expanduser, cmd)

        # Set up environment variables
        env = os.environ.copy()
        for k, v in addEnv.iteritems():
            env[k] = os.path.expanduser(v)

        # Set up death pipe
        if exitSig:
            dr, dw = os.pipe()
            def preexec():
                import fcntl, struct
                flags = fcntl.fcntl(dr, fcntl.F_GETFL)
                O_ASYNC = 020000
                fcntl.fcntl(dr, fcntl.F_SETFL, flags | O_ASYNC)
                fcntl.fcntl(dr, fcntl.F_SETOWN, os.getpid())
                fcntl.fcntl(dr, fcntl.F_SETSIG, exitSig)
                os.close(dw)
        else:
            dr = dw = None
            preexec = None

        # Create subprocess
        if LOG_COMMANDS:
            print >> sys.stderr, \
                "=%s= %s" % (self.__name, " ".join(map(shellEscape, cmd)))
        try:
            # Ugh.  Popen as of Python 2.6 isn't thread-safe.  See
            # Python issue 2320.  A better solution would be
            # close_fds=True, but that fails for some reason (XXX
            # track down).
            with popenLock:
                p = subprocess.Popen(cmd, stdin = pstdin, stdout = pstdout,
                                     stderr = pstderr, preexec_fn = preexec,
                                     shell = shell, cwd = cwd, env = env)
        except:
            if dw:
                os.close(dw)
            raise
        finally:
            if dr:
                os.close(dr)

        # Return Process object
        pobj = Process(cmd, p, dw)
        if wait:
            pobj.wait(wait == CHECKED)
        return RPCProxy(pobj)

    def procList(self):
        procs = {}
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            info = {}

            try:
                info["cmdline"] = file(os.path.join("/proc", pid, "cmdline")).read().split("\0")
                info["exe"] = os.readlink(os.path.join("/proc", pid, "exe"))
                info["status"] = {}
                for l in file(os.path.join("/proc", pid, "status")):
                    k, v = l.split(":", 1)
                    info["status"][k] = v.strip()
            except EnvironmentError, e:
                if e.errno == errno.ENOENT or e.errno == errno.EACCES:
                    continue
                raise
            procs[int(pid)] = info
        return procs

    def kill(self, pid, sig):
        os.kill(pid, sig)

    def writeFile(self, path, data, noCheck = False, append = False):
        if not noCheck:
            path = self.__safePath(path)
        else:
            self.__makedirs(os.path.dirname(path))
        file(path, "a" if append else "w").write(data)

    def readFile(self, path):
        return file(path).read()

    def readGzipFile(self, path):
        f = gzip.open(path, 'rb')
        content = f.read()
        f.close()
        return content

def main():
    sys.stdout = sys.stderr
    RPCServer(RemoteHost()).serve()

if __name__ == "__main__":
    main()
