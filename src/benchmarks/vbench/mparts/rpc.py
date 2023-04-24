from __future__ import with_statement

import sys, os, traceback, Queue, threading, signal
import cPickle as pickle

__all__ = ["RPCError", "RPCProxy", "RPCServer", "RPCClient",
           "print_remote_exception", "print_remote_exc"]

class RPCError(BaseException):
    pass

class RPCProxy(object):
    def __init__(self, obj):
        self._obj = obj
        self._id = None
        self.__methods = set()
        for k in dir(obj):
            if k.startswith("_"):
                continue
            v = getattr(obj, k)
            if hasattr(v, "im_func"):
                self.__methods.add(k)
        self.__tname = obj.__class__.__name__

    def __getstate__(self):
        if self._id == None:
            raise RPCError("RPCProxy's cannot be pickled")
        return (self._id, self.__methods, self.__tname)

    def __setstate__(self, (id, methods, tname)):
        self._obj = None
        self._id = id
        self.__methods = methods
        self.__tname = tname

    def _createProxy(self, client):
        dct = {}
        id = self._id
        for name in self.__methods:
            def bindName(name):
                def f(oself, *args, **kwargs):
                    return client._call(id, name, args, kwargs)
                f.func_name = name
                return f
            if name in dct:
                raise RPCError("Duplicate RPC method %s" % name)
            dct[name] = bindName(name)
        return type(self.__tname + "Proxy", (object,), dct)()

class SystemRoot(object):
    def __init__(self, userRoot):
        self.__userRoot = userRoot

    def getUserRoot(self):
        return RPCProxy(self.__userRoot)

class RPCServer(object):
    def __init__(self, root, to = sys.stdout, fro = sys.stdin):
        self.__oids = {0: SystemRoot(root)}
        self.__toFile = to
        self.__to = pickle.Pickler(to, pickle.HIGHEST_PROTOCOL)
        self.__fro = pickle.Unpickler(fro)
        self.__sendLock = threading.Lock()

    def __handle(self, rid, oid, name, args, kwargs):
        try:
            obj = self.__oids.get(oid)
            if obj == None:
                raise RPCError("Unknown oid %s" % oid)
            val = getattr(obj, name)(*args, **kwargs)
        except:
            typ, val, tb = sys.exc_info()
            tbLines = traceback.format_tb(tb)
            val.remote_traceback = tbLines
            rep = (rid, None, (typ, val))
        else:
            if isinstance(val, RPCProxy):
                val._id = len(self.__oids)
                self.__oids[val._id] = val._obj
            rep = (rid, val, None)
        with self.__sendLock:
            self.__to.dump(rep)
            self.__toFile.flush()
            self.__to.clear_memo()

    def serve(self):
        rootProxy = RPCProxy(self.__oids[0])
        rootProxy._id = 0
        self.__to.dump(rootProxy)
        self.__toFile.flush()
        while True:
            try:
                req = self.__fro.load()
            except EOFError:
                return
            t = threading.Thread(target = self.__handle,
                                 args = req)
            t.setDaemon(True)
            t.start()

def print_remote_exception(typ, val, tb):
    if tb:
        print >> sys.stderr, 'Traceback (most recent call last):'
        traceback.print_tb(tb)
    if val and hasattr(val, "remote_traceback"):
        print >> sys.stderr, 'Remote traceback:'
        print "".join(val.remote_traceback)[:-1]
    print "".join(traceback.format_exception_only(typ, val))[:-1]

def print_remote_exc():
    typ, val, tb = sys.exc_info()
    print_remote_exception(typ, val, tb)

class RPCClient(object):
    def __init__(self, to, fro):
        self.__toFile = to
        self.__to = pickle.Pickler(to, pickle.HIGHEST_PROTOCOL)
        self.__fro = pickle.Unpickler(fro)
        self.__rid = 0
        self.__sroot = self.__fro.load()._createProxy(self)

        self.__outstanding = {}
        self.__oLock = threading.Lock()

        # We run the send loop in a thread to protect it from SIGINT.
        # We protect the receive loop similarly, though that needs to
        # be threaded anyway.
        self.__sendQ = Queue.Queue(1)
        t = threading.Thread(target = self.__sendLoop,
                             name = "RPCClient send loop")
        t.setDaemon(True)
        t.start()

        t = threading.Thread(target = self.__recvLoop,
                             name = "RPCClient recv loop")
        t.setDaemon(True)
        t.start()

        self.r = self.__sroot.getUserRoot()

    def close(self):
        self.__sendQ.put(None)

    def __sendLoop(self):
        while True:
            v = self.__sendQ.get()
            if v == None:
                self.__toFile.close()
                self.__sendQ = None
                return
            self.__to.dump(v)
            self.__toFile.flush()
            self.__to.clear_memo()

    def __recvLoop(self):
        while True:
            try:
                rid, val, exc = self.__fro.load()
            except:
                typ, val, _ = sys.exc_info()
                with self.__oLock:
                    for pw, cell in self.__outstanding.values():
                        cell[0] = (None, (typ, val))
                        os.write(pw, "x")
                    self.__outstanding = None
                # Re-raise exception unless its an EOFError
                try:
                    raise
                except EOFError:
                    return

            with self.__oLock:
                pw, cell = self.__outstanding.pop(rid, (None, None))
                if pw:
                    cell[0] = (val, exc)
                    os.write(pw, "x")
                else:
                    print >> sys.stderr, "Discarding reply %d" % rid

    def _call(self, oid, name, args, kwargs):
        # Python threads suck and most operations are not
        # interruptable, so we use a wake-up pipe instead
        pr, pw = os.pipe()
        cell = [None]
        with self.__oLock:
            rid = self.__rid
            self.__rid += 1
            self.__outstanding[rid] = (pw, cell)
        req = (rid, oid, name, args, kwargs)
        try:
            self.__sendQ.put(req)
            os.read(pr, 1)
            val, exc = cell[0]
        finally:
            with self.__oLock:
                if self.__outstanding and rid in self.__outstanding:
                    del self.__outstanding[rid]
            os.close(pr)
            os.close(pw)
        if exc:
            raise exc[0], exc[1]
        if isinstance(val, RPCProxy):
            return val._createProxy(self)
        return val
