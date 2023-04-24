__all__ = ["TableDesc", "Row", "Table"]

class TableDesc(object):
    __slots__ = ["__nameFromIdx", "__idxFromName", "__getitem__", "idxFromName"]

    def __init__(self, *colNames):
        self.__nameFromIdx = colNames
        self.__idxFromName = {}
        for n, name in enumerate(colNames):
            assert isinstance(name, basestring)
            self.__idxFromName[name] = n
        self.idxFromName = self.__idxFromName.__getitem__
        self.__getitem__ = self.__nameFromIdx.__getitem__

    def __len__(self):
        return len(self.__nameFromIdx)

    def __iter__(self):
        return iter(self.__nameFromIdx)

    def __repr__(self):
        return "TableDesc%r" % tuple(self)

    def __eq__(self, other):
        if self is other:
            return True
        if not isinstance(other, TupleDesc):
            return False
        return self.__nameFromIdx == other.__nameFromIdx

    def __ne__(self, other):
        # Geez.
        return not (self == other)

    def __hash__(self):
        return hash(self.__nameFromIdx)

class Row(object):
    __slots__ = ["__desc", "__data", "__length_hint__",
                 "__getitem__", "__iter__", "__getslice__"]

    def __init__(self, desc, data):
        self.__desc = desc
        self.__data = tuple(data)
        # Shh, don't tell anyone
        self.__length_hint__ = len(self.__data)
        # Forward methods
        self.__getitem__ = self.__data.__getitem__
        self.__iter__ = self.__data.__iter__
        self.__getslice__ = self.__data.__getslice__

    def __getattr__(self, name):
        try:
            idx = self.__desc.idxFromName(name)
        except KeyError:
            return object.__getattr__(self, name)
        return self.__data[idx]

    def __str__(self):
        out = ["{"]
        for n, name in enumerate(self.__desc):
            if n != 0:
                out.append(", ")
            out.append(name)
            out.append(": %r" % self.__data[n])
        out.append("}")
        return "".join(out)

    def __repr__(self):
        return "Row(%r, %r)" % (self.__desc, self.__data)

    def __eq__(self, other):
        if not isinstance(other, Row):
            return False
        return self.__desc == other.__desc and self.__data == other.__data

    def __ne__(self, other):
        return not (self == other)

    def __hash__(self):
        return hash(self.__desc) ^ hash(self.__data)

class Table(object):
    def __init__(self, desc, extra = None):
        self.__desc = desc
        if extra == None:
            extra = {}
        self.__extra = extra
        assert hasattr(self, "tuples")

    # Accessors

    @property
    def desc(self):
        return self.__desc

    @property
    def extra(self):
        return self.__extra

    def __iter__(self):
        for row in self.tuples():
            yield Row(self.__desc, row)

    # Constructors

    @staticmethod
    def fromIterable(iterable, desc, extra = None):
        """Construct a table from an iterable object.  This object
        must support repeated iteration (it must not be, for example,
        a generator)."""

        return IterTable(iterable, desc, extra)

    def __compileProj(self, proj, varDict, colSet = None):
        if isinstance(proj, tuple):
            name, proj = proj
            haveName = True
        else:
            haveName = False
        if isinstance(proj, basestring):
            idx = self.__desc.idxFromName(proj)
            if colSet != None:
                colSet.add(idx)
            return name if haveName else proj, "i[%d]" % idx
        if isinstance(proj, int):
            if colSet != None:
                colSet.add(proj)
            return name if haveName else self.__desc[proj], "i[%d]" % proj
        if hasattr(proj, "__call__"):
            if not haveName or not name:
                raise ValueError("Function projection must provide column name")
            fvar = "_%d" % len(varDict)
            varDict[fvar] = proj
            # XXX Create just one row object
            return name, "%s(Row(desc, i))" % fvar
        raise ValueError("Projection of unknown type %s" % type(proj))

    def project(self, *projs):
        """Construct a new table by projecting each row of this table
        into a new row.  Each argument specifies a projection that
        will produce zero or more columns in the output table.  Each
        argument is a tuple of the form (output column name,
        projection).  A basic projection is a string or integer, in
        which case that column will be copied verbatim into the output
        table.  For basic projections, the output column name may be
        omitted.  The projection can also be a function that takes a
        Row object and returns the value for that column in the new
        row.

        An argument may also be the string "*", which stands for all
        columns of this table that have not appeared as a basic
        projection in some other argument.  To suppress columns from
        appearing in the output table despite "*", list them with None
        as their output column name."""

        varDict = {"Row" : Row, "desc" : self.__desc,
                   "__builtins__" : __builtins__}
        colSet = set()
        exprs = [self.__compileProj(proj, varDict, colSet)
                 if proj != "*" else ("*", None)
                 for proj in projs]

        # Expand "*"'s and build output column names
        outNames = []
        outExprs = []
        for outName, expr in exprs:
            if outName == None:
                pass
            elif outName == "*":
                for n, col in enumerate(self.__desc):
                    if n not in colSet:
                        outNames.append(col)
                        outExprs.append("i[%d]," % n)
            else:
                outNames.append(outName)
                outExprs.append(expr + ",")

        code = compile("(" + "".join(outExprs) + ")", "<generated>", "eval")

        return ProjectionTable(self, code, varDict, TableDesc(*outNames))

    # Rendering

    def renderText(self):
        return "\n".join(toText(self))

class IterTable(Table):
    def __init__(self, iterable, desc, extra):
        Table.__init__(self, desc, extra)
        self.__it = iterable

    def tuples(self):
        return iter(self.__it)

    def __len__(self):
        return len(self.__it)

class ProjectionTable(Table):
    def __init__(self, base, code, varDict, desc):
        Table.__init__(self, desc, base.extra)
        self.__base = base
        self.__code = code
        self.__varDict = varDict

    def tuples(self):
        for tup in self.__base.tuples():
            self.__varDict["i"] = tup
            yield eval(self.__code, self.__varDict)

    def __len__(self):
        return len(self.base)

def toText(x):
    if x == None:
        return [""]
    if isinstance(x, basestring):
        return x.split("\n")
    if isinstance(x, int) or isinstance(x, long):
        return [str(x)]
    if isinstance(x, float):
        return ["%g" % x]
    if isinstance(x, list):
        res = []
        for e in x:
            lines = toText(e)
            for i, l in enumerate(lines):
                if i == 0:
                    res.append("- " + l)
                else:
                    res.append("  " + l)
        return res
    if isinstance(x, tuple):
        liness = map(toText, x)
        if all(len(lines) == 1 for lines in liness):
            return [", ".join(lines[0] for lines in liness)]
        raise ValueError("Can't render multi-line value in tuple")
    if isinstance(x, Table):
        # Get alignment hints
        align = []
        for colName in x.desc:
            a = x.extra.get(colName + "Align", "l")
            if a == "l":
                align.append(str.ljust)
            elif a == "r":
                align.append(str.rjust)
            else:
                raise ValueError("Unknown alignment type %s" % a)
        # Textify all cells
        cells = [[[col] for col in x.desc]]
        if len(x.desc):
            cells[0][0][0] = "# " + cells[0][0][0]
        for r in x.tuples():
            cells.append(map(toText, r))
        # Figure out column spacing
        widths = [0] * len(x.desc)
        for row in cells:
            for i, cell in enumerate(row):
                if len(cell):
                    cellWidth = max(len(line) for line in cell)
                else:
                    cellWidth = 0
                widths[i] = max(widths[i], cellWidth)
        # Add one space of padding between columns
        widths = [w + 1 for w in widths]
        # Render rows
        res = []
        for row in cells:
            height = max(len(lines) for lines in row)
            for n in range(height):
                resLine = []
                for i, cell in enumerate(row):
                    line = cell[n] if n < len(cell) else ""
                    resLine.append(align[i](line, widths[i]))
                res.append("".join(resLine))
        return res
    raise ValueError("Don't know how to render %s" % type(x))
