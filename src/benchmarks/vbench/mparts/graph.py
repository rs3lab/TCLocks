import os, subprocess, tempfile, re

__all__ = ["Gnuplot"]

SQUOTE_RE = re.compile("[^\\'\n]*$")
def quote(s):
    if SQUOTE_RE.match(s):
        return "'" + s + "'"
    raise NotImplementedError("Don't know how to escape %r" % s)

def mkStrSetter(prop):
    def setter(self, val):
        self.attrs[prop] = val
        self("set %s %s" % (prop, quote(val)))
    return property(fset = setter)

def mkGenSetter(prop):
    def setter(self, val):
        self.attrs[prop] = val
        self("set %s %s" % (prop, val))
    return property(fset = setter)

class Gnuplot(object):
    dirPath = None
    def __init__(self, pdf=None, archive=False):
        self.__cmds = []
        self.__plot = []
        self.__dataFile = None
        self.__dataIndex = 0
        self.__dataDir = None

        # make it accessible from setter
        self.attrs = {}
        
        if pdf != None:
            self("set terminal pdfcairo")
            self("set output \"%s\"" % pdf)

        cwd = None
        if archive:
            if Gnuplot.dirPath is None:
                Gnuplot.dirPath = tempfile.mkdtemp(prefix="gnuplot")
            cwd = self.__dataDir = Gnuplot.dirPath

        self.__g = subprocess.Popen(["gnuplot", "--persist"],
                                    stdin=subprocess.PIPE,
                                    cwd=cwd)
            
    def __call__(self, cmd):
        self.__cmds.append(cmd)

    def __flush(self):
        fd = None
        if self.__dataDir is not None:
            gp = self.attrs.get("title", "plot") + ".gp"
            fd = open(os.path.join(self.__dataDir, gp), "w'")
        for cmd in self.__cmds:
            if fd:
                print >> fd, cmd
            else:
                print cmd
            # Unfortunately, there's no way to check if the command
            # succeeded, short of running gnuplot in a PTY.
            print >> self.__g.stdin, cmd
        self.__cmds[:] = []
        if fd:
            fd.close()
            print("# Saved %s in %s" % (gp, self.__dataDir))
            
    xlabel = mkStrSetter("xlabel")
    ylabel = mkStrSetter("ylabel")
    ytics = mkGenSetter("ytics")
    yrange = mkGenSetter("yrange")
    y2label = mkStrSetter("y2label")
    y2tics = mkGenSetter("y2tics")
    y2range = mkGenSetter("y2range")
    title = mkStrSetter("title")

    def addData(self, data, axis = None, title = None, with_ = None,
                linecolor = None, pointtype = None):

        # archive all data
        if self.__dataDir is not None \
           and self.__dataFile is None:
            assert(self.attrs.get("title", None) is not None)
            pn = self.attrs["title"] + ".dat"
            self.__dataFile = (open(os.path.join(self.__dataDir, pn), "w"), pn)

        # fallback
        if self.__dataFile == None:
            # We intentionally leave these temp files around because
            # Gnuplot re-reads them whenever we zoom the graph.
            fd, path = tempfile.mkstemp(prefix = "gnuplot")
            f = os.fdopen(fd, "w")
            self.__dataFile = (f, path)
        else:
            f, path = self.__dataFile

        for tup in data:
            print >> f, "\t".join(map(str, tup))
        print >> f
        print >> f
        cmd = "%r index %d" % (path, self.__dataIndex)
        self.__dataIndex += 1

        if with_ == None and (linecolor != None or pointtype != None):
            with_ = "points"
        if axis != None:
            cmd += " axis " + axis
        if title != None:
            cmd += " title " + quote(title)
        if with_ != None:
            cmd += " with " + with_
        if linecolor != None:
            cmd += " linecolor %d" % linecolor
        if pointtype != None:
            cmd += " pointtype %d" % pointtype
        self.__plot.append(cmd)
        return self

    def plot(self):
        if not len(self.__plot):
            raise ValueError("Nothing to plot")
        self.__dataFile = None
        self.__dataIndex = 0
        self("plot " + ",\\\n  ".join(self.__plot))
        self.__flush()
        self.__plot[:] = []
