# Testing

Tests live beside the code they test, in files ending `_test.sword`.
`shield test` builds them together with the package, runs them, and reports.

```sword
// std/strings/strings_test.sword
package strings

import "std/testing"

func TestTrimSpace(mut t *testing.T) !void {
    t.Equal(TrimSpace("  ada  "), "ada")
    t.Equal(TrimSpace("\t\nada\r\n"), "ada")
    t.Equal(TrimSpace(""), "")
}
```

```sh
$ shield test std/strings
ok    6 tests  (0ms)
```

A test is a function whose name starts with `Test`, taking one `*testing.T` and
returning `!void`. Nothing registers it: the compiler finds it and writes the
entry point that runs it.

The test file says `package strings`, so it sees the package's own names —
`TrimSpace`, not `strings.TrimSpace`. It is inside the package, which also means
it can test what the package does not export.

**A `_test.sword` file is left out of every ordinary build.** That is what lets
the tests sit next to the code: a program that imports `std/strings` does not
carry them, and cannot see them.

## Failing

Two shapes, and the difference is whether the test goes on:

| | keeps going | ends the test |
|---|---|---|
| a value | `t.Equal(got, want)` | `try t.Must(got, want)` |
| a condition | `t.Check(ok)` | `try t.Require(ok)` |
| a message | `t.Failf(...)` | `try t.Fatalf(...)` |

Keeping going is the usual case, so one run reports everything that is wrong
rather than the first thing. End the test when the rest of it cannot go on
without the value — a length that is about to be indexed, a pointer about to be
followed.

"Ends the test" is not a special power. `Must`, `Require` and `Fatalf` return an
error, and the `try` in front of them carries it out of the test function — the
same way `try` leaves any other function early. There is no unwinding here and
nothing to remember: if you wrote `try`, a failure stops, and if you did not, it
does not.

```sword
func TestParse(mut t *testing.T) !void {
    doc := try Parse([]u8(src), t.Mem)   // a failure here ends the test too
    root := doc.Root()

    try t.Must(doc.Kind(root), Kind.Object)   // nothing below means anything otherwise
    t.Equal(doc.Len(root), 5)                 // wrong count, but carry on
    t.Equal(doc.GetText(root, "name"), "ada")
}
```

What a test says is collected in its own memory. If that runs out, the run says
so once, after the test's output, rather than failing the checks that were
trying to report.

A test that returns an error it did not mean to — the `try Parse` above — is
reported as a failure with the error's name:

```
--- FAIL  TestParseU64
      returned error.NotANumber
```

`t.Equal` compares through `any`, so it takes integers, floats, bools, strings
and enums, and prints both sides when they differ. Strings come out quoted,
because a failure about whitespace is unreadable otherwise:

```
--- FAIL  TestTrimSpace
      got "\ta", want "a"
```

Anything `any` cannot carry — a struct, a slice — you compare yourself and report
with `t.Failf`.

## They run together

Tests run at the same time — 64 at once by default. Sword has no global
variables, so two tests cannot reach each other except through a port, a file or
the clock; the reason other languages run tests in order by default is not here.

```sh
shield test std/http          # all at once
shield test std/http -p 1     # one at a time
shield test std/http -p 4     # four at a time
```

Each test has its own memory and its own output, and a test's output is written
out in one piece when it ends, so two failing at the same time do not shuffle
their lines together.

Reach for `-p 1` when a test needs something it cannot share: a fixed port, a
directory, the working directory.

## Memory

`t.Mem` is an allocator the test can use, its own for each test. So a test
allocates and never frees:

```sword
func TestListGrows(mut t *testing.T) !void {
    mut xs := try NewList[i64](t.Mem, 2)
    for i in 0..100 {
        try xs.Push(i64(i) * 2)
    }
    t.Equal(xs.Len(), 100)
}
```

It is an arena of 256 KiB. A test that needs more builds its own, the same way
any other Sword code would.

## Subtests

`t.Run` gives a section of a test its own label, which is what shows up when it
fails:

```
--- FAIL  TestParseU64/12a
      accepted it
```

There are no closures, so a subtest body cannot capture anything. `t.RunWith`
hands it one value through `t.Arg`:

```sword
func TestParseU64(mut t *testing.T) !void {
    bad := [4]string{"", "12a", "-1", " 7"}
    for s in bad {
        try t.RunWith(s, rejects, s)
    }
}

func rejects(mut t *testing.T) !void {
    ParseU64(t.Arg.Text) catch {
        return
    }
    t.Failf("accepted it")
}
```

A case with more than one value in it does not fit in `t.Arg`. Then keep the loop
in the test and name the case in the message, which costs one line and reads just
as well:

```sword
struct trimCase {
    input string
    want  string
}

func TestTrimSpaceTable(mut t *testing.T) !void {
    cases := [2]trimCase{
        trimCase{input: "  ada  ", want: "ada"},
        trimCase{input: "   ", want: ""},
    }
    for c in cases {
        got := TrimSpace(c.input)
        if got != c.want {
            t.Failf("{}: got {} want {}", c.input, got, c.want)
        }
    }
}
```

## Skipping

```sword
try t.Skip("needs a network")
```

Ends the test without failing it, and says why on the way out. A skip nobody can
see is a test nobody ever comes back to.

## Running them

```sh
shield test std/strings          # one package
shield test ./mypkg              # any directory
shield test foo_test.sword       # a single file
shield test std/json -o /tmp/jt  # keep the binary
```

`-run` narrows a run to the tests it names, and can be given more than once:

```sh
shield test std/strings -run TestTrimSpace -run TestEqual
```

The name is the whole name, not a pattern. One that is not a test in the package
stops the build, because a typo that ran nothing and passed would read as good
news.

The exit status is zero only when nothing failed, so it drops into a build
without ceremony. `make test` in this repository runs the compiler's own tests,
the language server's, and then every `std/*/*_test.sword` package.

Testing a program rather than a library works too: its `main` is set aside for
the run, not called.

## Simulation

A test of concurrent code usually passes. The order its tasks ran in was one of
many, the one this machine happened to pick, and the order that breaks it may
come up once in ten thousand runs — on a server, at night. `-sim` runs each test
under a different order on purpose, once per seed, and when one fails it says
which seed. Take a counter that two tasks bump by reading it and writing it
back:

```sword
func bump(counter *atomic[u64]) {
    seen := counter.Load()
    counter.Store(seen + 1)
}

func TestLostUpdate(mut t *testing.T) !void {
    mut counter := atomic[u64](0)
    scope {
        spawn bump(&counter)
        spawn bump(&counter)
    }
    t.Equal(counter.Load(), u64(2))
}
```

Each access is atomic and the race checker has nothing to say, but when both
tasks read before either writes, one update is lost. On a real machine that
takes a coincidence; under `-sim` it takes a few seeds. A package holding that
test, one that takes two locks in opposite orders, and one that opens a socket:

```sh
shield test ./mypkg -sim               # every test, seeds 1 to 100
shield test ./mypkg -sim -seeds 5000   # more
```

```
FAIL  TestLostUpdate under seed 2
     --- FAIL  TestLostUpdate
           got 1, want 2
      reproduce: shield test -sim -seed 2 -run TestLostUpdate ./mypkg
FAIL  TestOppositeLocks under seed 2: deadlock
     sword: deadlock: every task is waiting and nothing can wake one (simulated, seed 2)
      reproduce: shield test -sim -seed 2 -run TestOppositeLocks ./mypkg
SKIP  TestNetwork: uses the network, which -sim does not simulate yet
FAIL  4 tests, 2 failed, 1 skipped, 100 seeds each
```

The line it prints is the whole of what it takes to have that failure again, as
many times as it takes to understand it. That is the point: a concurrency bug
you can reproduce is an ordinary bug.

What happens under `-sim`:

- **One thread, and the seed decides.** At every point where a task could give
  way to another — a `lock`, a `spawn`, a channel, a wait, and every operation
  on an `atomic` — the scheduler picks what runs next from among everything that
  could, and the seed makes the pick. The same seed makes the same choices, so
  the same run.
- **A virtual clock.** Time moves only when every task is waiting on it, and
  then straight to the next deadline: a test with a thirty-second timeout, or an
  hour-long sleep, finishes at once. The wall clock reads 2000-01-01 00:00:00 UTC
  when the run starts and moves with it.
- **Random bytes come from the seed.** `crypto.Random` gives the same bytes
  under the same seed, so a run that makes a token makes the same token again.
  A program run for real must never have `SWORD_SIM_SEED` set.
- **A deadlock is an answer.** When every task waits for something only another
  task could give it and no clock will change that, the run stops and says so,
  where a real program would simply hang.
- **Files are read and written as usual**, on the spot, so the same contents
  give the same run.

Any program runs the same way with `SWORD_SIM_SEED`, without `shield test`:

```sh
SWORD_SIM_SEED=48213 ./server
```

A build with `--sim` also switches tasks at atomic operations, which is where
two tasks' order over an atomic lives; `shield test -sim` always builds that
way, and without the flag an ordinary build is exactly what it always was.

The network is not simulated yet. A test that opens a socket is skipped under
`-sim` with a line that says so — running it on a real socket would make a seed
mean nothing. A call out of the language through `extern` is compiled with a
warning, since the simulator can neither schedule around it nor play it back.

## Benchmarks

A `Benchmark...` function taking one `*B` is found the same way a test is, and
`-bench` runs those instead of the tests:

```sword
import "std/testing"

func BenchmarkSort(mut b *testing.B) !void {
    mut xs := mem.Alloc[u64](b.Mem, 1024) orelse return error.OutOfMemory
    fill(xs)
    b.ResetTimer()
    for i in 0..b.N {
        collections.Sort[u64](xs, less)
        b.Keep(i64(xs[0]))
    }
}
```

```sh
shield test std/collections -bench
shield test std/collections -bench -run BenchmarkSort
```

```
benchmarking: median of 9 samples, spread half the quartile range
BenchmarkSort                   31417.81 ns/op  0.3%  N=3200
```

`b.N` is not yours to pick. The harness calls the body with a small one, sees
how long that took, and scales until a measurement lasts about a tenth of a
second; then it takes nine of those and reports the middle one. The spread is
half the distance between the quartiles, as a share of the median — quartiles
rather than the ends, because one sample that landed on another program's
scheduling says nothing about the code and moves the range further than
anything else. Two benchmarks whose medians differ by less than their spread
have not been told apart.

`b.ResetTimer` throws away what has been timed so far, which is what setup
wants; `b.StopTimer` and `b.StartTimer` bracket something in the middle of a
body that should not count. `b.Keep` is where a result nothing else reads
goes — without it the work can be dropped for having no effect.

`b.Mem` is an arena of the benchmark's own, reset before every sample, and what
the body takes from it after the last `ResetTimer` is reported as bytes per
operation. That number needs no estimating here: nothing allocates without
being handed an allocator.

Benchmarks run one at a time, in the order they are written. Two at once would
be measuring each other.

## What is missing

**A simulated network.** `-sim` skips a test that uses a real socket; sockets
that deliver, delay, lose and reorder under the seed are still to come.

**Line numbers.** A failure names the test and the subtest, not the file and
line: there is no way to ask for the caller's position. Subtest labels are the
substitute, and they are usually enough.

