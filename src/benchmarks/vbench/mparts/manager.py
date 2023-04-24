from __future__ import with_statement

import sys, os, time
import cPickle as pickle

from util import Progress, maybeMakedirs

__all__ = ["Manager", "Task", "WaitForUser", "ResultPath", "generateManagers"]

STOPPED, RUNNING = range(2)

def prNotice(urg, text):
    if urg == 0:
        print text
    elif urg == 1:
        print "\x1b[1;36m%s\x1b[0m" % text
    elif urg >= 2:
        print "\x1b[1;32m%s\x1b[0m" % text

class Manager(object):
    """A Manager stores a collection of Tasks, provides a way for
    those tasks to discover other tasks, and manages the life cycle of
    those tasks."""

    def __init__(self):
        # Tasks in start order
        self.__tasks = []
        # dict(class, [(dict(string, val), task)]) for all base
        # classes of all tasks.
        self.__clsMaps = {}
        # Set of task names
        self.__names = set()

    def __iadd__(self, task):
        """Add a task to the manager.  The task's name must be unique
        within the manager (which generally means its class and keys
        must be unique.  This also accepts a list of tasks."""

        if isinstance(task, list):
            for t in task:
                self.__add(t)
        else:
            self.__add(task)
        return self

    def __add(self, task):
        if task.name in self.__names:
            raise ValueError("A task with the name %s already exists" %
                             task.name)
        self.__names.add(task.name)

        self.__tasks.append(task)
        for bc in type(task).__mro__:
            self.__clsMaps.setdefault(bc, []).append((task._keys, task))

        return self

    def tasks(self):
        """Return a list of all tasks, in start order."""

        return self.__tasks[:]

    def find(self, cls, **keys):
        """Find all tasks that subclass the given class and whose
        keys match all given keys."""

        res = []
        objs = self.__clsMaps.get(cls, [])
        keyPairs = keys.items()
        for okeys, obj in objs:
            for k, v in keyPairs:
                if okeys.get(k) != v:
                    break
            else:
                res.append(obj)
        return res

    def find1(self, cls, **keys):
        """Like find, but return just one task and raise a KeyError if
        not exactly one matches the query."""

        v = self.find(cls, **keys)
        if len(v) == 1:
            return v[0]
        if len(v) == 0:
            raise KeyError("No tasks matched query")
        raise KeyError("Multiple tasks matched query")

    def __callMeMethod(self, fn):
        if (fn.im_func.func_code.co_argcount == 1
            if hasattr(fn, "im_func") else
            fn.func_code.co_argcount == 0):
            return fn()
        return fn(self)

    def __start(self):
        """Start all tasks in this manager."""

        for obj in self.__tasks:
            if obj._state == RUNNING:
                if obj._manager != self:
                    raise ValueError("Task %s is running in another manager" %
                                     obj.name)
                continue
            if hasattr(obj, "start") and obj.start:
                with Progress("Starting %s" % obj.name):
                    self.__callMeMethod(obj.start)
            obj._state = RUNNING
            obj._manager = self

    def __wait(self):
        """Wait on waitable started tasks in this manager."""

        for obj in self.__tasks:
            if obj._state == RUNNING and hasattr(obj, "wait") and obj.wait:
                assert obj._manager == self
                with Progress("Waiting on %s" % obj.name):
                    self.__callMeMethod(obj.wait)

    def __stop(self):
        """Stop all running tasks in this manager in reverse of the
        order they were started in."""

        for obj in reversed(self.__tasks):
            if obj._state == STOPPED:
                continue
            assert obj._manager == self
            if hasattr(obj, "stop") and obj.stop:
                with Progress("Stopping %s" % obj.name):
                    self.__callMeMethod(obj.stop)
            obj._state = STOPPED
            obj._manager = None

    def __reset(self):
        """Reset all tasks in this manager in their stop order.  Tasks
        are reset regardless of their current state and are always put
        in the STOPPED state.  Exceptions will be printed and
        ignored."""

        for obj in reversed(self.__tasks):
            assert obj._manager == None or obj._manager == self
            if hasattr(obj, "reset") and obj.reset:
                with Progress("Resetting %s" % obj.name):
                    try:
                        self.__callMeMethod(obj.reset)
                    except:
                        sys.excepthook(*sys.exc_info())
            obj._state = STOPPED
            obj._manager = None

    def run(self):
        """Run all tasks in the manager through their full life-cycle.
        Start all tasks, then wait on tasks, then stop all tasks.  If
        an exception is raised during any of this, reset all tasks and
        re-raise the exception."""

        try:
            self.__start()
            self.__wait()
            self.__stop()
        except:
            self.__reset()
            raise

class Task(object):
    """A Task is an object in a Manager.  It is identified in a
    manager by its class and by some set of keys.  Every task has a
    string name derived from its class and keys, which is stored in
    the 'name' field.

    == Life-cycle management ==

    A Task can have four optional methods for interacting with
    manager state changes, all of which are passed the acting manager:
    * start - Transition from stopped to running state.
    * wait  - After being started, pause until the task is ready to
              be stopped.
    * stop  - Transition from running to stopped state.
    * reset - Transition from any state to stopped state.
    As a convenience, these methods can omit the activity manager
    argument.

    reset() is called on *all* tasks in response to an exception
    being raised during *any* part of a run (start, wait, or stop).
    Otherwise, runs are terminated using the stop() method.

    == Configuration/results recording ==

    Each Task has a set of fields that are recorded for each
    experiment.  See Task.getInfo.
    """

    __slots__ = ["name", "classNames", "_state", "_manager", "_keys"]
    __info__ = ["name", "classNames"]

    def __init__(self, **keys):
        """Initialize the task.  The keyword arguments specify the
        keys that can be used to look up this task in a manager.  This
        will construct a default name from the class name and the
        keys."""

        # classNames is used to look up objects in info dumps
        self.classNames = [c.__name__ for c in type(self).__mro__]
        name = self.__class__.__name__
        for k, v in keys.iteritems():
            name += ".%s-%s" % (k, v)
        self.name = name

        self._state = STOPPED
        self._manager = None
        self._keys = keys

    def __str__(self):
        return self.name

    def getInfo(self):
        """Return the information dictionary for this task.  Each task
        class may has a class field '__info__' which lists names of
        fields.  The info dictionary will consist of these field names
        and their values.  If any of the names start with '*', then
        that field must contain a dictionary that will be merged into
        the info dictionary.

        The returned dictionary will be cleaned so that it contains
        only regular Python types.  If any objects of user type are
        encountered, they must have a 'toInfoValue' method that returns
        the object represented as some basic Python type."""

        names = set()
        for cls in reversed(type(self).__mro__):
            if hasattr(cls, "__info__"):
                names.update(cls.__info__)
        info = {}
        for name in names:
            if name.startswith("*"):
                for k, v in getattr(self, name[1:]).iteritems():
                    info[k] = self.__primitize(v)
            else:
                info[name] = self.__primitize(getattr(self, name))
        return info

    def __primitize(self, o):
        """Convert o into a pickle-safe primitive value, recursively."""

        if o == None:
            return o
        if isinstance(o, (basestring, bool, float, int, long, complex)):
            # Atomic types
            return o
        c = self.__primitize
        if isinstance(o, dict):
            return dict((c(k), c(v)) for k, v in o.iteritems())
        if isinstance(o, list):
            return map(c, o)
        if isinstance(o, tuple):
            return tuple(map(c, o))
        if hasattr(o, "toInfoValue"):
            return c(o.toInfoValue())
        # XXX We could just string anything else
        raise ValueError("%r in %s is an invalid info type" %
                         (type(o), self.name))

    def log(self, msg):
        print "[%s] %s" % (self.name, msg)

    def setInfoAttrs(self, cls, dct):
        """For any variable in both cls.__info__ and dct, set that
        attribute in this object to the value in dct.  Passing in
        'locals()' for dct is a quick way to set lots of info
        attributes in a class' constructor."""

        for v in cls.__info__:
            if v in dct:
                setattr(self, v, dct[v])

class WaitForUser(Task):
    def wait(self):
        raw_input("Press enter to continue...")

class ResultPath(Task):
    """A ResultPath tracks where to store the results of an
    experiment."""

    def __init__(self, basepath, noTS = False):
        """Create a result directory in a subdirectory of basepath
        named by the current timestamp.  If noTS is True, inhibit the
        timestamp."""

        Task.__init__(self)
        if noTS:
            self.__path = basepath
        else:
            ts = time.strftime("%Y%m%d-%H%M%S")
            self.__path = os.path.join(basepath, ts)

    def getPath(self):
	return self.__path

    def ensure(self):
        """Ensure that the result directory exists and return its
        path."""

        maybeMakedirs(self.__path)
        return self.__path

    def stop(self, m):
        """Dump the info dictionaries of all entities into the result
        directory and print the path of the result directory."""

        p = self.ensure()

        # Gather info
        info = []
        for o in m.tasks():
            info.append(o.getInfo())

        pickle.dump(info, file(os.path.join(p, "info"), "w"))

        prNotice(2, "Results in: " + p)

def generateManagers(resultRoot, configSpace):
    ts = time.strftime("%Y%m%d-%H%M%S")
    basepath = os.path.join(resultRoot, "incomplete", ts)
    finalpath = os.path.join(resultRoot, ts)
    pointerpath = os.path.join(resultRoot, "latest")

    # Set up initial results directory
    maybeMakedirs(basepath)
    incomplete = os.path.join(basepath, "incomplete")
    file(incomplete, "w").close()
    prNotice(2, "Starting results in: %s" % basepath)

    # Walk configurations
    start = time.time()
    for n, config in enumerate(configSpace):
        # Show progress
        now = time.time()
        if n > 0 and start != now:
            print "Time so far: %s\tETA: %s" % \
                (time.strftime("%H:%M:%S", time.gmtime(now - start)),
                 time.strftime("%H:%M:%S", time.gmtime((now - start)*(len(configSpace) - n)/n)))

        # Construct result path
        path = basepath
        for (cfgName, cfgVal, cfgConst) in config._values():
            if cfgConst:
                continue
            path = os.path.join(path, "%s-%s" % (cfgName, cfgVal))
        prNotice(1, "*** Starting configuration %d/%d (%s) ***" % \
                     (n+1, len(configSpace), path[len(basepath):].lstrip("/")))

        m = Manager()
        m += ResultPath(path, noTS = True)
        yield (m, config)

    # All configurations are complete
    os.unlink(incomplete)
    os.rename(basepath, finalpath)
    if os.path.exists(pointerpath):
        os.unlink(pointerpath)
    try:
        os.symlink(ts, pointerpath)
    except OSError, e:
        print ""
    prNotice(2, "All results in: %s" % finalpath)
