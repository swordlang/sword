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
"""


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

    def task(self):
        """One spawn or loop of spawns: (lines, [(reads, writes, inner race)])."""
        r = self.r
        k = r.randrange(11)
        t = self.next_tag()
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

    def scope(self):
        lines, tasks = [], []
        for _ in range(self.r.randrange(1, 4)):
            l, t = self.task()
            lines += l
            tasks += t
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
        for _ in range(self.r.randrange(1, 3)):
            lines, w = self.scope()
            why = why or w
            body.append("scope {")
            body += ["    " + l for l in lines]
            body.append("}")
        out = [HEADER, "func main() !int {",
               "    mut buf := [%d]i64{}" % N, "    mut oth := [%d]i64{}" % N,
               "    for i in 0..buf.len {", "        buf[i] = i64(i) +% 1",
               "        oth[i] = i64(i) *% 3 +% 2", "    }"]
        out += ["    mut %s := i64(0)" % n for n in self.results]
        if self.needs_arena:
            out += ["    mut backing := [8192]u8{}", "    mut arena := mem.NewArena(backing[..])"]
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
print(json.dumps({"racy": why is not None, "why": why}))
