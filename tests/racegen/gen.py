"""Small Sword programs for the race checker, with the truth beside them.

Every program lends pieces of two arrays to tasks in one or more scopes. The
generator knows, element by element, what each task reads and writes, so it
knows whether the program has a race before the compiler says anything.
Bounds the compiler cannot fold are computed by cut(), whose value the
generator knows, so "racy" is exact and not a guess on the cautious side.

    python3 gen.py <seed> <out.sword>     prints {"racy": ..., "why": ...}

tests/racegen.sh runs a range of seeds through the compiler.
"""
import json
import random
import sys

N = 24
BASES = ("buf", "oth")

HEADER = """import "std/io"
import "std/mem"

struct Box {
    xs []i64
}

func fill(mut a []i64, t i64) {
    for k in 0..a.len {
        a[k] = a[k] *% 31 +% t +% i64(k)
    }
}

func sum(a []i64, mut out *i64) {
    mut s := i64(0)
    for k in 0..a.len {
        s = s *% 7 +% a[k]
    }
    *out = s
}

func mix(mut a []i64, b []i64, t i64) {
    for k in 0..a.len {
        a[k] = a[k] *% 31 +% b[k % b.len] +% t
    }
}

func point(mut a []i64, i u64, t i64) {
    a[i] = a[i] *% 31 +% t
}

func both(mut a []i64, mut b []i64, t i64) {
    for k in 0..a.len {
        a[k] = a[k] *% 31 +% t
    }
    for k in 0..b.len {
        b[k] = b[k] *% 37 +% t
    }
}

func pair(mut a []i64, b []i64, t i64, mut out *i64) {
    scope {
        spawn fill(a, t)
        spawn sum(b, out)
    }
}

func pfill(mut a []i64, b []i64, t i64) {
    parallel for i in 0..a.len {
        a[i] = a[i] *% 31 +% b[(i + 1) % b.len] +% t
    }
}

func cut(n i64) i64 {
    return n / 2
}

func head(mut a []i64, n u64) []i64 {
    return a[0..n]
}

func wrap(mut a []i64) Box {
    return Box{xs: a}
}

func (b *Box) Whole() []i64 {
    return b.xs
}

func store(mut b *Box, mut a []i64) {
    b.xs = a
}

func locked(sh *shared[Box], t i64) {
    lock b := sh {
        fill(b.xs, t)
    }
}

func received(ch chan[Box], t i64) {
    for mut b := <-ch {
        fill(b.xs, t)
    }
}

func fillrows(mut rows [][]i64, t i64) {
    for j in 0..rows.len {
        fill(rows[j], t)
    }
}

func sumrows(rows [][]i64, mut out *i64) {
    mut s := i64(0)
    for j in 0..rows.len {
        for k in 0..rows[j].len {
            s = s *% 7 +% rows[j][k]
        }
    }
    *out = s
}
"""

# Only in the programs that use it: every iteration writes through its row,
# which the checker refuses whatever the rows are.
PROWS = """
func prows(mut rows [][]i64, t i64) {
    parallel for i in 0..rows.len {
        rows[i][0] = rows[i][0] *% 31 +% t
    }
}
"""


# Only in the programs that use them: a store through one parameter, and a
# task reading through another, which is the same memory when the caller
# hands both one array.
STORES = """
func storeread(mut ps [][]i64, qs [][]i64, mut z []i64, t i64, mut out *i64) {
    ps[0] = z
    scope {
        spawn sumrows(qs, out)
        spawn fill(z, t)
    }
}

func storeblock(mut ps [][]i64, qs [][]i64, t i64, mut out *i64, mut a mem.Allocator) {
    mut w := mem.Alloc[i64](a, 2) orelse return
    w[0] = t
    w[1] = t
    ps[0] = w
    scope {
        spawn sumrows(qs, out)
        spawn fill(w, t)
    }
}
"""

# Only in the programs that use it: where the address of a helper's local
# slice can be put.
REF = """
struct Ref {
    p *[]i64
}
"""


# How a helper's memory leaves it, and how that memory is named on the way
# out. The helper is written per program, so its name and shape vary.
LEAVES = ("send", "keep", "lock", "store")
NAMES = ("cut", "named", "head", "wrap")


class View:
    """A slice expression and the elements of a base it covers."""

    def __init__(self, expr, base, lo, hi, named):
        self.expr, self.base, self.lo, self.hi, self.named = expr, base, lo, hi, named

    def cells(self, lo=0, hi=None):
        hi = self.hi - self.lo if hi is None else hi
        return {(self.base, self.lo + k) for k in range(lo, hi)}

    @property
    def len(self):
        return self.hi - self.lo


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.setup = []
        self.n = 0
        self.tag = 1
        self.results = []
        self.holders = []
        self.needs_arena = False
        self.needs_prows = False
        # buf and oth come from the arena in half of the programs: a channel
        # may only carry memory that does not live in main's frame.
        self.heap = self.r.random() < 0.5
        self.helpers = []
        self.escapes = None
        self.needs_stores = False
        self.needs_ref = False
        # A call the parent makes inside a scope, after its spawns.
        self.pending = None
        # Some programs hold one of the store shapes and nothing else, so
        # that no other race in the scope hides whether it is seen.
        self.focus = self.r.random() < 0.15

    def fresh(self, p):
        self.n += 1
        return "%s%d" % (p, self.n)

    def next_tag(self):
        self.tag += 1
        return self.tag

    def span(self, length):
        a = self.r.randrange(0, length)
        b = self.r.randrange(a + 1, length + 1)
        return a, b

    def view(self, depth=0):
        r = self.r
        if depth >= 2 or r.random() < 0.35:
            base = r.choice(BASES) if r.random() < 0.3 else "buf"
            k = r.random()
            if k < 0.3:
                return View(base + "[..]", base, 0, N, False)
            if k < 0.5:
                # A bound the compiler cannot fold: cut() is a call.
                c = self.fresh("k")
                self.setup.append("%s := u64(cut(%d))" % (c, N))
                if r.random() < 0.5:
                    return View("%s[0..%s]" % (base, c), base, 0, N // 2, False)
                return View("%s[%s..]" % (base, c), base, N // 2, N, False)
            a, b = self.span(N)
            return View("%s[%d..%d]" % (base, a, b), base, a, b, False)
        inner = self.view(depth + 1)
        if r.random() < 0.3:
            k2 = r.randrange(4)
            name = self.fresh("f")
            if k2 == 0:
                c = r.randrange(1, inner.len + 1)
                self.setup.append("mut %s := head(%s, %d)" % (name, inner.expr, c))
                return View(name, inner.base, inner.lo, inner.lo + c, True)
            if k2 == 1:
                self.setup.append("mut %s := wrap(%s)" % (name, inner.expr))
                return View(name + ".xs", inner.base, inner.lo, inner.hi, False)
            if k2 == 2:
                b = self.fresh("b")
                self.setup.append("mut %s := Box{xs: %s}" % (b, inner.expr))
                self.setup.append("mut %s := %s.Whole()" % (name, b))
                return View(name, inner.base, inner.lo, inner.hi, True)
            self.setup.append("mut %s := Box{xs: oth[0..0]}" % name)
            self.setup.append("store(&%s, %s)" % (name, inner.expr))
            return View(name + ".xs", inner.base, inner.lo, inner.hi, False)
        k = r.random()
        if k < 0.3:
            name = self.fresh("v")
            self.setup.append("mut %s := %s" % (name, inner.expr))
            v = View(name, inner.base, inner.lo, inner.hi, True)
            if r.random() < 0.4 and v.len > 1:
                a, b = self.span(v.len)
                return View("%s[%d..%d]" % (name, a, b), v.base, v.lo + a, v.lo + b, False)
            return v
        if k < 0.5:
            name = self.fresh("b")
            self.setup.append("mut %s := Box{xs: %s}" % (name, inner.expr))
            if r.random() < 0.5:
                p = self.fresh("p")
                self.setup.append("mut %s := &%s" % (p, name))
                return View(p + ".xs", inner.base, inner.lo, inner.hi, False)
            return View(name + ".xs", inner.base, inner.lo, inner.hi, False)
        if k < 0.75:
            other = self.view(depth + 1)
            name = self.fresh("r")
            self.setup.append("mut %s := [2][]i64{%s, %s}" % (name, inner.expr, other.expr))
            if r.random() < 0.5:
                return View(name + "[0]", inner.base, inner.lo, inner.hi, False)
            return View(name + "[1]", other.base, other.lo, other.hi, False)
        if inner.len > 1:
            a, b = self.span(inner.len)
            name = self.fresh("v")
            self.setup.append("mut %s := %s" % (name, inner.expr))
            return View("%s[%d..%d]" % (name, a, b), inner.base, inner.lo + a, inner.lo + b, False)
        return inner

    def named(self):
        v = self.view()
        if v.named:
            return v
        name = self.fresh("v")
        self.setup.append("mut %s := %s" % (name, v.expr))
        return View(name, v.base, v.lo, v.hi, True)

    def result(self):
        name = self.fresh("res")
        self.results.append(name)
        return name

    def leave_address(self):
        """A helper that lets out the address of its local slice: a cell of
        its frame, wherever the slice points."""
        fn = self.fresh("leave")
        if self.r.random() < 0.5:
            self.helpers.append("func %s(mut given []i64) *[]i64 {\n    mut ys := given\n    return &ys\n}\n" % fn)
            self.setup += ["if %s(oth[0..2]).len == 0 {" % fn, "    return 1", "}"]
        else:
            self.needs_ref = True
            self.helpers.append("func %s(mut r *Ref, mut given []i64) {\n    mut ys := given\n    r.p = &ys\n}\n" % fn)
            anchor, h = self.fresh("anchor"), self.fresh("hold")
            self.setup += ["mut %s := oth[0..0]" % anchor, "mut %s := Ref{p: &%s}" % (h, anchor),
                           "%s(&%s, oth[0..2])" % (fn, h)]
        self.escapes = self.escapes or "%s lets out the address of its local slice" % fn

    def leave(self):
        """A call to a helper that puts memory where it outlives the helper:
        its own frame's, which must be refused, or what it was given, which
        need not be."""
        r = self.r
        if r.random() < 0.2:
            self.leave_address()
            return
        how, naming = r.choice(LEAVES), r.choice(NAMES)
        own = r.random() < 0.6
        fn = self.fresh("leave")
        src = "local[..]" if own else "given"
        body = ["    mut local := [4]i64{1, 2, 3, 4}"]
        if naming == "cut":
            src = "local[1..3]" if own else "given[0..given.len]"
        elif naming == "named":
            body.append("    mut ys := %s" % src)
            src = "ys"
        elif naming == "head":
            src = "head(%s, 2)" % src
        else:
            body.append("    mut w := wrap(%s)" % src)
            src = "w.xs"
        holder = {"send": "ch chan[Box]", "keep": "mut b *Box",
                  "lock": "sh *shared[Box]", "store": "mut b *Box"}[how]
        put = {"send": ["    ch <- Box{xs: %s} catch {}" % src],
               "keep": ["    b.xs = %s" % src],
               "lock": ["    lock b := sh {", "        b.xs = %s" % src, "    }"],
               "store": ["    store(b, %s)" % src]}[how]
        self.helpers.append("func %s(%s, mut given []i64) {\n%s\n    local[0] = given[0]\n}\n" %
                            (fn, holder, "\n".join(body + put)))
        h = self.fresh("hold")
        given = "oth[0..2]"
        if how == "send":
            self.needs_arena = True
            self.setup.append("mut %s := try chan[Box](&arena, 1)" % h)
            arg = h
        elif how == "lock":
            self.setup.append("mut %s := shared[Box](Box{xs: oth[0..0]})" % h)
            arg = "&" + h
        else:
            self.setup.append("mut %s := Box{xs: oth[0..0]}" % h)
            arg = "&" + h
        self.setup.append("%s(%s, %s)" % (fn, arg, given))
        if own:
            self.escapes = self.escapes or "%s puts its frame's memory where it outlives the call" % fn
        elif how == "send" and not self.heap:
            # What it was given is main's frame, and that goes on no channel.
            self.escapes = self.escapes or "%s sends main's frame on a channel" % fn

    def task(self):
        """One spawn or loop of spawns: (lines, [(reads, writes, inner race)])."""
        r = self.r
        k = r.randrange(15)
        if self.focus:
            k = r.choice((13, 14))
        while (k == 10 and not self.heap) or (k == 14 and self.pending is not None):
            k = r.randrange(15)
        t = self.next_tag()
        if k == 13:
            return self.stored_through_name(t)
        if k == 14:
            self.pending = self.stored_through_param(t)
            return [], []
        if k in (11, 12):
            # A slice of slices: the rows are elements of their own, but the
            # memory each one leads to can be another row's.
            views = [self.view() for _ in range(r.randrange(2, 5))]
            rows = self.fresh("rr")
            self.setup.append("mut %s := [%d][]i64{%s}" % (rows, len(views), ", ".join(v.expr for v in views)))
            named = self.fresh("rs")
            self.setup.append("mut %s := %s[..]" % (named, rows))
            if k == 12:
                self.needs_prows = True
                heads = [(v.base, v.lo) for v in views]
                inner = "prows's iterations write the same element" if len(set(heads)) < len(heads) else None
                return ["spawn prows(%s, %d)" % (named, t)], [(set(), set(heads), inner)]
            c = r.randrange(1, len(views) + 1)
            lines = ["for o, part in %s.chunks(%d) {" % (named, c),
                     "    spawn fillrows(part, %d)" % t, "}"]
            acc = []
            for lo in range(0, len(views), c):
                cells = set()
                for v in views[lo:lo + c]:
                    cells |= v.cells()
                acc.append((set(), cells, None))
            return lines, acc
        if k == 9:
            if not self.holders or r.random() < 0.5:
                a = self.view()
                name = self.fresh("sh")
                self.setup.append("mut %s := shared[Box](Box{xs: %s})" % (name, a.expr))
                self.holders.append((name, a, t))
            name, a, t = r.choice(self.holders)
            return ["spawn locked(&%s, %d)" % (name, t)], [(set(), a.cells(), None, name)]
        if k == 10:
            a = self.view()
            name = self.fresh("ch")
            self.needs_arena = True
            self.setup.append("mut %s := try chan[Box](&arena, 2)" % name)
            self.setup.append("try %s <- Box{xs: %s}" % (name, a.expr))
            self.setup.append("close(%s)" % name)
            n = r.randrange(1, 3)
            return (["spawn received(%s, %d)" % (name, t)] * n,
                    [(set(), a.cells(), None, name)] * n)
        if k == 0:
            a = self.view()
            return ["spawn fill(%s, %d)" % (a.expr, t)], [(set(), a.cells(), None)]
        if k == 1:
            a = self.view()
            out = self.result()
            return ["spawn sum(%s, &%s)" % (a.expr, out)], [(a.cells(), set(), None)]
        if k == 2:
            a, b = self.view(), self.view()
            reads = b.cells(0, min(a.len, b.len))
            return ["spawn mix(%s, %s, %d)" % (a.expr, b.expr, t)], [(reads, a.cells(), None)]
        if k == 3:
            a = self.view()
            i = r.randrange(a.len)
            return ["spawn point(%s, %d, %d)" % (a.expr, i, t)], [(set(), a.cells(i, i + 1), None)]
        if k == 4:
            a, b = self.view(), self.view()
            return ["spawn both(%s, %s, %d)" % (a.expr, b.expr, t)], [(set(), a.cells() | b.cells(), None)]
        if k == 5:
            a, b = self.view(), self.view()
            out = self.result()
            inner = "pair writes and reads the same element" if a.cells() & b.cells() else None
            return (["spawn pair(%s, %s, %d, &%s)" % (a.expr, b.expr, t, out)],
                    [(b.cells(), a.cells(), inner)])
        if k == 6:
            a, b = self.view(), self.view()
            # Iteration i writes a[i] and reads b[(i + 1) % b.len].
            writer = {(a.base, a.lo + i): i for i in range(a.len)}
            inner = None
            for i in range(a.len):
                cell = (b.base, b.lo + (i + 1) % b.len)
                if cell in writer and writer[cell] != i:
                    inner = "pfill's iterations write and read the same element"
            reads = {(b.base, b.lo + (i + 1) % b.len) for i in range(a.len)}
            return ["spawn pfill(%s, %s, %d)" % (a.expr, b.expr, t)], [(reads, a.cells(), inner)]
        if k == 7:
            v = self.named()
            c = r.randrange(1, max(2, v.len))
            pieces = [(v.lo + o, min(v.hi, v.lo + o + c)) for o in range(0, v.len, c)]
            if r.random() < 0.5:
                b = self.view()
                lines = ["for off, part in %s.chunks(%d) {" % (v.expr, c),
                         "    spawn mix(part, %s, %d)" % (b.expr, t), "}"]
                acc = []
                for lo, hi in pieces:
                    n = hi - lo
                    acc.append((b.cells(0, min(n, b.len)), {(v.base, x) for x in range(lo, hi)}, None))
                return lines, acc
            lines = ["for off, part in %s.chunks(%d) {" % (v.expr, c),
                     "    spawn fill(part, %d)" % t, "}"]
            return lines, [(set(), {(v.base, x) for x in range(lo, hi)}, None) for lo, hi in pieces]
        # k == 8: chunks inside chunks, the inner one cut from the piece or from the whole.
        v = self.named()
        c1 = r.randrange(2, max(3, v.len + 1))
        c2 = r.randrange(1, c1)
        whole = r.random() < 0.5
        src = v.expr if whole else "p1"
        lines = ["for o1, p1 in %s.chunks(%d) {" % (v.expr, c1),
                 "    for o2, p2 in %s.chunks(%d) {" % (src, c2),
                 "        spawn fill(p2, %d)" % t, "    }", "}"]
        acc = []
        for o1 in range(0, v.len, c1):
            lo1, hi1 = (0, v.len) if whole else (o1, min(v.len, o1 + c1))
            for o2 in range(lo1, hi1, c2):
                acc.append((set(), {(v.base, v.lo + x) for x in range(o2, min(hi1, o2 + c2))}, None))
        return lines, acc

    def rows(self):
        """An array of slices in main, and what each of its rows covers."""
        views = [self.view() for _ in range(self.r.randrange(1, 4))]
        name = self.fresh("sr")
        self.setup.append("mut %s := [%d][]i64{%s}" % (name, len(views), ", ".join(v.expr for v in views)))
        return name, [v.cells() for v in views]

    def stored_through_name(self, t):
        """A slice stored into a row through one name of the array, and the
        array read through another, taken before the store or after it."""
        r = self.r
        rows, cells = self.rows()
        x = self.view()
        j = r.randrange(len(cells))
        form = r.randrange(4)
        if form == 0:
            self.setup.append("%s[%d] = %s" % (rows, j, x.expr))
            reader = rows + "[..]"
        elif form == 1:
            ps = self.fresh("ps")
            self.setup += ["mut %s := %s[..]" % (ps, rows), "%s[%d] = %s" % (ps, j, x.expr)]
            reader = rows + "[..]"
        elif form == 2:
            reader = self.fresh("qs")
            self.setup += ["%s := %s[..]" % (reader, rows), "%s[%d] = %s" % (rows, j, x.expr)]
        else:
            ps, reader = self.fresh("ps"), self.fresh("qs")
            self.setup += ["mut %s := %s[..]" % (ps, rows), "%s := %s[..]" % (reader, rows),
                           "%s[%d] = %s" % (ps, j, x.expr)]
        old = cells[j]
        cells[j] = x.cells()
        out = self.result()
        lines = ["spawn sumrows(%s, &%s)" % (reader, out)]
        acc = [(set().union(*cells), set(), None)]
        k = r.random()
        if k < 0.4:
            lines.append("spawn fill(%s, %d)" % (x.expr, t))
            acc.append((set(), x.cells(), None))
        elif k < 0.7:
            # What the row held before the store, which the reader no longer
            # reaches through that row.
            w = self.view()
            lines.append("spawn fill(%s, %d)" % (w.expr, t))
            acc.append((set(), w.cells(), None))
        return lines, acc

    def stored_through_param(self, t):
        """A call that stores into one parameter's array and has a task read
        another's, given the same array or two different ones."""
        r = self.r
        self.needs_stores = True
        rows, cells = self.rows()
        same = r.random() < 0.5
        pa = self.fresh("pa")
        self.setup.append("mut %s := %s[..]" % (pa, rows))
        if same:
            pb, bcells = self.fresh("pb"), cells
            self.setup.append("%s := %s[..]" % (pb, rows))
        else:
            other, bcells = self.rows()
            pb = other + "[..]"
        out = self.result()
        if r.random() < 0.5:
            x = self.view()
            line = "storeread(%s, %s, %s, %d, &%s)" % (pa, pb, x.expr, t, out)
            after = [x.cells()] + bcells[1:] if same else bcells
            reads = set().union(*after)
            inner = "storeread reads what it writes" if reads & x.cells() else None
            return [line], [(reads, x.cells(), inner)]
        self.needs_arena = True
        line = "storeblock(%s, %s, %d, &%s, &arena)" % (pa, pb, t, out)
        reads = set().union(*(bcells[1:] if same else bcells))
        inner = "storeblock reads the block it writes" if same else None
        return [line], [(reads, set(), inner)]

    def scope(self):
        lines, tasks = [], []
        self.pending = None
        for _ in range(1 if self.focus else self.r.randrange(1, 4)):
            l, t = self.task()
            lines += l
            tasks += t
        # The parent's own call runs beside every task spawned before it,
        # and they all are.
        if self.pending is not None:
            lines += self.pending[0]
            tasks += self.pending[1]
        why = None
        tasks = [t if len(t) == 4 else t + (None,) for t in tasks]
        for reads, writes, inner, _ in tasks:
            why = why or inner
        for i in range(len(tasks)):
            for j in range(i + 1, len(tasks)):
                ri, wi, _, hi = tasks[i]
                rj, wj, _, hj = tasks[j]
                # Users of one lock take turns; receivers of one channel get
                # its one value once.
                if hi is not None and hi == hj:
                    continue
                if wi & (rj | wj) or wj & ri:
                    why = why or "two tasks of one scope reach the same element, one writing"
        return lines, why

    def program(self):
        body, why = [], None
        if self.r.random() < 0.3 and not self.focus:
            self.leave()
        for _ in range(1 if self.focus else self.r.randrange(1, 3)):
            lines, w = self.scope()
            why = why or w
            body.append("scope {")
            body += ["    " + l for l in lines]
            body.append("}")
        why = why or self.escapes
        out = [HEADER + (PROWS if self.needs_prows else "") + (STORES if self.needs_stores else "") +
               (REF if self.needs_ref else "") + "".join("\n" + h for h in self.helpers),
               "func main() !int {"]
        if self.heap or self.needs_arena:
            out += ["    mut backing := [8192]u8{}", "    mut arena := mem.NewArena(backing[..])"]
        if self.heap:
            out += ["    mut buf := mem.Alloc[i64](&arena, %d) orelse return 1" % N,
                    "    mut oth := mem.Alloc[i64](&arena, %d) orelse return 1" % N]
        else:
            out += ["    mut buf := [%d]i64{}" % N, "    mut oth := [%d]i64{}" % N]
        out += ["    for i in 0..buf.len {", "        buf[i] = i64(i) +% 1",
               "        oth[i] = i64(i) *% 3 +% 2", "    }"]
        out += ["    mut %s := i64(0)" % n for n in self.results]
        out += ["    " + l for l in self.setup]
        out += ["    " + l for l in body]
        out += ["    mut h := u64(14695981039346656037)",
                "    for i in 0..buf.len {",
                "        h = (h ^ u64(buf[i])) *% 1099511628211",
                "        h = (h ^ u64(oth[i])) *% 1099511628211",
                "    }"]
        out += ["    h = (h ^ u64(" + n + ")) *% 1099511628211" for n in self.results]
        out += ['    try io.Printf("{x}\\n", h)', "    return 0", "}"]
        return "\n".join(out) + "\n", why


g = Gen(int(sys.argv[1]))
text, why = g.program()
open(sys.argv[2], "w").write(text)
print(json.dumps({"racy": why is not None, "why": why, "leaves": g.escapes}))
