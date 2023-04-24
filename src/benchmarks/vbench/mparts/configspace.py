"""A configuration space represents a set of configurations, where a
configuration is an assignment of some set of variables to values.
Configuration spaces are constructed from sums and products of smaller
configuration spaces, where the smallest non-unital configuration
space assigns a single variable.

Configurations spaces support the iteration protocol and produce
Config objects.  The variables in a configuration are accessible as
attributes of the Config object.

A configuration space can be thought of like a polynomial over
variable assignments.  Once the polynomial has been fully expanded to
a sum of products, each term gives a single configuration in the
space.  For example, if you wanted the space to contain all
combinations of assigning 'a' to 1 and 2 and assigning 'b' to 3 and 4,
you could construct the space
  ([a=1]+[a=2])*([b=3]+[b=4])
fully multiplied out, this yields
  [a=1]*[b=3] + [a=1]*[b=4] + [a=2]*[b=3] + [a=2]*[b=4]
Each term in the result gives a single configuration in the space.
See ConfigSpace.mk for a convenient configuration space constructor.

Abstractly, configuration spaces form a non-commutative algebraic
semiring over variable assignments, with a few extra operators thrown
in for convenience.
"""

__all__ = ["Config", "ConfigSpace"]

def getArg(name, kwarg):
    """Return a single (name, value) pair from a keyword argument
    dictionary, raising a TypeError if there is not exactly one
    keyword argument."""

    if len(kwarg) != 1:
        raise TypeError("%s takes exactly 1 keyword argument" % name)
    return kwarg.iteritems().next()

class Config(object):
    """A configuration.  The configuration variables are accessible as
    attributes of a Config object."""

    def __init__(self, vals):
        """Construct a configuration.  vals should be a list of (name,
        value, constant) tuples, one for each configuration
        variable."""

        self.__vals = vals
        self.__dct = {}
        for (name, val, _) in vals:
            self.__dct[name] = val

    def _values(self):
        """Retrieve an ordered list of configuration variables and
        values.  Returns a list of (name, value, constant) tuples.
        The order of these variables corresponds to how the original
        configuration space was constructed.  'constant' indicates
        whether or not the configuration variable takes on multiple
        values within the original configuration space."""

        return self.__vals

    def __getattr__(self, name):
        """Retrieve the given configuration variable."""

        try:
            return self.__dct[name]
        except KeyError:
            raise AttributeError("'Config' object has no attribute '%s'" % name)

    def __str__(self):
        return "*".join("%s%c%s" % (n, "#" if c else "=", v)
                        for n, v, c in self.__vals)

class ConfigAtom(object):
    """A ConfigAtom assigns a single configuration variable to a
    single value.  These are the basic elements out of which the
    configuration space is constructed."""

    def __init__(self, name, val):
        if name.startswith("_"):
            raise ValueError("Configuration variable %r starts with _" % name)
        self.name, self.val = name, val

    def __eq__(self, other):
        if not isinstance(other, ConfigAtom):
            return False
        return self.name == other.name and self.val == other.val

    def __hash__(self):
        return hash(self.name) ^ hash(self.val)

    def __str__(self):
        return "%s=%s" % (self.name, self.val)

class ConfigTerm(object):
    """A ConfigTerm is a product of ConfigAtoms."""

    def __init__(self, factors):
        """Construct a ConfigTerm from a tuple of ConfigAtom's."""

        assert isinstance(factors, tuple)
        self.__factors = factors

    def getLeftmostName(self):
        """Return the variable name of the left-most ConfigAtom."""

        return self.__factors[0].name

    def __iter__(self):
        """Iterate over the ConfigAtom's in this term."""

        return self.__factors.__iter__()

    def __eq__(self, other):
        if not isinstance(other, ConfigTerm):
            return False
        return self.__factors == other.__factors

    def __hash__(self):
        return hash(self.__factors)

    def __str__(self):
        return "*".join(map(str, self.__factors))

    def __mul__(self, other):
        # XXX This could be more efficient (resulting in potentially
        # much more efficiency in producing spaces) using rope-like
        # tricks.  Depends on how I do the constant flag.

        return ConfigTerm(self.__factors + other.__factors)

def varmapUpdate(varmap, other):
    overlap = None
    for var, vals in other.iteritems():
        if not overlap and var in varmap:
            overlap = var
        varmap.setdefault(var, set()).update(vals)
    return overlap

def varmapCopy(varmap):
    return dict((k, set(v)) for k, v in varmap.items())

NONCONST_NONCE = object()

class ConfigSpace(object):
    """A ConfigSpace is a set of configurations, where each
    configuration assigns some set of variables to values.
    Configuration spaces are constructed by combining smaller
    configuration spaces either by addition or multiplication."""

    def __init__(self, name, val, nonConst = False):
        """Construct a configuration space that assigns a single
        variable to a single value.  If nonConst, then even if the
        variable takes on only one value in the configuration space,
        it will be treated as non-constant."""

        atom = ConfigAtom(name, val)
        term = ConfigTerm((atom,))
        vals = [val]
        if nonConst:
            # We force this variable to be non-constant by shoving
            # another (guaranteed unique) value into its value set.
            vals.append(NONCONST_NONCE)
        self.__init([term], None, {name: frozenset(vals)}, False)

    @staticmethod
    def zero():
        cs = ConfigSpace.__new__(ConfigSpace)
        cs.__init([], None, {}, False)
        return cs

    @staticmethod
    def unit():
        cs = ConfigSpace.__new__(ConfigSpace)
        cs.__init([ConfigTerm(())], None, {}, False)
        return cs

    @staticmethod
    def mk(nonConst = False, **kwarg):
        """DWIM constructor for a ConfigSpace containing one variable
        assigned to potentially many values.  Beyond the nonConst
        argument, there must be exactly one keyword argument.  This
        keyword argument specifies the configuration variable.  Its
        value can be a list or a tuple, in which case the returned
        space will assign that variable to each of the values in the
        list.  Otherwise, the returned space will consist of only the
        one assignment of the variable to the argument value.  For
        nonConst, see ConfigSpace.__init__."""

        # We retain the public constructor because it is better for
        # programmatic use.

        # We could allow multiple keywords and simply form the product
        # of their spaces, but we don't know the order of keyword
        # arguments, so this could be confusing.
        name, val = getArg("mk", kwarg)
        if isinstance(val, (list, tuple)):
            terms = [ConfigSpace(name, x, nonConst = nonConst) for x in val]
            return ConfigSpace.union(*terms)
        return ConfigSpace(name, val, nonConst)

    @staticmethod
    def __mk(*args):
        cs = ConfigSpace.__new__(ConfigSpace)
        cs.__init(*args)
        return cs

    def __init(self, terms, termset, varmap, mixed):
        """Initialize this ConfigSpace.  Internally, a ConfigSpace is
        a sum of ConfigTerms, which are products of ConfigAtoms.
        terms is the list of ConfigTerms that form this ConfigSpace.
        termset is the set of all ConfigTerm's.  termset may be None,
        indicating that the termset should be constructed on-demand.
        varmap is a dictionary mapping each variable name to a set of
        values it takes on, which is used to determine which variables
        are constant and for some consistency checks.  mixed indicates
        whether it's acceptable to have left-most assignments to
        different variables."""

        self.__terms = terms
        self.__termset = termset
        self.__varmap = varmap
        self.__mixed = mixed

        if termset != None:
            assert len(termset) == len(terms)

    def __getTermset(self):
        """Return the set of all terms in the configuration
        expression, constructing it if necessary."""

        if self.__termset == None:
            self.__termset = frozenset(self.__terms)
            assert len(self.__termset) == len(self.__terms)
        return self.__termset

    def __str__(self):
        return "+".join(map(str, self.__terms))

    def __len__(self):
        """Return the number of configurations in this configuration
        space."""

        return len(self.__terms)

    def __iter__(self):
        """Iterate over the Config's in this configuration space."""

        for term in self.__terms:
            vals = [(atom.name, atom.val, len(self.__varmap[atom.name]) == 1)
                    for atom in term]
            yield Config(vals)

    def __add__(self, other):
        """Return the sum of the left and right configuration spaces.
        See ConfigSpace.union."""

        return self.union(other)

    def union(*summands, **kwargs):
        """Return a configuration space consisting of the union of the
        configurations in the given configuration spaces.  The given
        configuration spaces must be disjoint and their left-most
        assignments must all be to the same variable.  The optional
        'mixed' keyword argument, if True, means the left-most
        assignments may be to different variables."""

        # Unpack keyword arguments
        def getKwargs(mixed = False):
            return mixed
        mixed = getKwargs(**kwargs)

        # We're mixed if any summand is mixed
        mixed = mixed or any(s.__mixed for s in summands)

        if not mixed:
            # Ensure that the first factor name in all terms of the
            # summands agree.  Since none are mixed, it suffices to
            # compare just one term from each.
            name = None
            for summand in summands:
                if not len(summand):
                    continue
                sname = summand.__terms[0].getLeftmostName()
                if name == None:
                    name = sname
                if name != sname:
                    raise ValueError("Sum mixes assignments to %s and %s" %
                                     (name, sname))

        # Check that all terms are unique.
        termset = set()
        for summand in summands:
            overlap = termset.intersection(summand.__getTermset())
            if overlap:
                raise ValueError("Duplicate configuration: %s" % overlap.pop())
            termset.update(summand.__getTermset())

        # Construct sum space
        terms = []
        varmap = {}
        for summand in summands:
            terms.extend(summand.__terms)
            varmapUpdate(varmap, summand.__varmap)
            # If we're mixed, then we have to mark the left-most
            # variables as non-constant or they may get flattened.
            if mixed:
                for term in summand.__terms:
                    varmap[term.getLeftmostName()].add(NONCONST_NONCE)
        return ConfigSpace.__mk(terms, termset, varmap, mixed)

    def __mul__(self, other):
        """Return the product of the left and right configuration
        spaces.  The resulting configuration space will contain every
        combination of some configuration from the left argument with
        some configuration from the right argument.  The set of
        variables assigned by the left and right arguments must be
        disjoint (otherwise, some configuration in the resulting space
        would assign twice to the same variable)."""

        return self.__product(other)

    def merge(self, other):
        """Return a configuration space like this one, but with the
        assignments from other substituting for any missing
        assignments in any configuration.  These variables will be
        appended to the variables in each configuration.  This can
        either be thought of as giving default values for unspecified
        variables, or as overriding existing variables."""

        terms = []
        varmap = varmapCopy(self.__varmap)

        for left in self.__terms:
            # Get the variable names defined on the left so we can
            # ignore them from the right.
            leftVars = frozenset(f.name for f in left)
            # As we remove overridden variables from the right,
            # different combinations of left and right terms may
            # result in the same term.  Deduplicate them.
            newTermSet = set()

            for right in other.__terms:
                # Get the variables from the right that are not
                # already defined on the left.
                newAtoms = [a for a in right if a.name not in leftVars]
                # Build up a new by adding in variables from the right
                newTerm = left * ConfigTerm(tuple(newAtoms))
                # Do we already have this term?
                if newTerm not in newTermSet:
                    # Nope.  It's fer reals.
                    terms.append(newTerm)
                    newTermSet.add(newTerm)
                    for newAtom in newAtoms:
                        varmap.setdefault(newAtom.name, set()).add(newAtom.val)

        return self.__mk(terms, None, varmap, False)

    def __product(self, other):
        """Return the product of the left and right configuration
        spaces.  It is an error for variables assigned by self and
        other to overlap."""

        # Check that the result will only assign to each variable
        # once.  We need only check for overlap in the variable sets,
        # since any overlap indicates that some term from self will
        # conflict with some term from other.  We construct the new
        # varmap at the same time.
        varmap = varmapCopy(self.__varmap)
        overlap = varmapUpdate(varmap, other.__varmap)
        if overlap:
            raise ValueError("Multiple assignments to %r" % overlap)

        # Construct product space
        terms = [ta*tb
                 for ta in self.__terms
                 for tb in other.__terms]
        return ConfigSpace.__mk(terms, None, varmap, False)

def testSpace(space, want):
    got = " ".join(map(str, space))
    if got != want:
        raise ValueError("Expected %r, got %r" % (want, got))

def testError(spaceThunk, err):
    try:
        space = spaceThunk()
    except ValueError:
        pass
    else:
        got = " ".join(map(str, space))
        raise ValueError("Expected %s error, got %r" % (err, got))

if __name__ == "__main__":
    mk = ConfigSpace.mk

    testSpace((mk(a=[1,2]) * mk(b=[1,2,3])) + mk(a=[3,4]),
              "a=1*b=1 a=1*b=2 a=1*b=3 a=2*b=1 a=2*b=2 a=2*b=3 a=3 a=4")

    # Test constant terms
    testSpace(mk(a=[1]) + mk(a=[2]), "a=1 a=2")
    testSpace(mk(a=[1]), "a#1")
    testSpace(mk(a=[1], nonConst = True), "a=1")

    # Test value overlap check
    testError(lambda: mk(a=[1]) * mk(a=[1]),
              "overlap")
    testError(lambda: mk(a=[1,2]) + mk(a=[2]),
              "overlap")
    testSpace((mk(a=1)*mk(b=1)) + (mk(a=1)*mk(b=2)),
              "a#1*b=1 a#1*b=2")

    # Test variable agreement check
    testError(lambda: mk(a=[1]) + mk(b=[1]),
              "variable agreement")
    testSpace(mk(a=[1]).union(mk(b=[1]), mixed = True),
              "a=1 b=1")

    # Test defaulting
    testSpace(mk(a=[1,2]).merge(mk(a=[3,4])),
              "a=1 a=2")
    testSpace(mk(a=[1]).merge(mk(a=[2,3])),
              "a#1")
    testSpace(mk(a=[1]).merge(mk(a=[2])),
              "a#1")
    testSpace(((mk(a=1)*mk(b=1)) + mk(a=2)).merge(mk(b=2)),
              "a=1*b=1 a=2*b=2")

    # Test printing
    assert str(mk(a=[1,2])*mk(b=[1,2])) == "a=1*b=1+a=1*b=2+a=2*b=1+a=2*b=2"

    # Test unit
    c = ConfigSpace.unit()
    c *= mk(a=1)
    c *= mk(b=1)
    testSpace(c, "a#1*b#1")
