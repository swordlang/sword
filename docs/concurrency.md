# Concurrency

Most languages give you threads and trust you. Sword gives you tasks and checks
you: a program where two tasks can touch the same memory at the same time does
not compile. This chapter is about how that works and what it costs you in
exchange.

Read [the tour](tour.md) first if you have not.

## Tasks and scopes

```sword
import "std/mem"

func shade(mut row []i64, gain i64) {
    for i in 0..row.len {
        row[i] *= gain
    }
}

func main() !int {
    mut backing := [4096]u8{}
    mut arena := mem.NewArena(backing[..])
    mut rows := mem.Alloc[i64](&arena, 64) orelse return error.OutOfMemory
    for i in 0..rows.len {
        rows[i] = 1
    }

    scope {
        for part in rows.chunks(8) {
            spawn shade(part, 3)
        }
    }

    return int(rows[0] + rows[63])
}
```
<!-- exits: 6 -->

`spawn` only exists inside `scope`, and the closing brace of the `scope` is the
join — it waits for every task it started. There is no handle to keep, nothing
to remember to wait on, and no way for a task to outlive the block that created
it.

That last part is the point. Because a task cannot survive its scope, the
compiler knows a slice lent to one stays valid for the task's whole life. It is
why you can hand out pieces of memory that live on the parent's stack, or in an
arena the parent will throw away right after.

### `main` is a task

The program starts as one, on a stack of its own, which is what lets `main`
wait: a read there puts the task down and the thread goes on, the same as
anywhere else. A program whose whole job is one loop over one descriptor — a
terminal editor reading keys — needs no `scope` around itself to do it.

It costs what a task costs. `main` gets `SWORD_STACK_KB` of stack, a megabyte
by default, rather than the eight the operating system hands a thread; a
`main` that keeps something enormous in a frame wants that setting raised. The
worker threads are another matter: they are started by the first `spawn`, so a
program that never spawns runs on the thread it came with.

### `spawn` takes a call, and only a call

```sword
spawn shade(part, 3)
```

Not a block, not a closure. The arguments are evaluated in the parent and
copied into the task.

This is a real restriction and it is deliberate. Everything a task can reach is
written on that one line, which is exactly what the race checker needs to look
at. A closure would hide the same information inside a body you would have to
read.

### A task returns nothing

There is no `Task[T]`, no handle, no `await`. A task writes into memory the
parent gave it:

```sword
scope {
    for off, part in results.chunks(16) {
        spawn compute(input[off..off+part.len], part)
    }
}
// results is complete here
```

That sounds limiting until you notice it removes a whole category of
synchronisation. The parent allocated the destination before spreading the
work, each task owns a disjoint piece of it, and the join is the only handoff.

Errors do still come back. A task running a function that returns `!void`
carries its failure out to the scope:

```sword
func check(xs []i64, limit i64) !void {
    for i in 0..xs.len {
        if xs[i] > limit {
            return error.TooBig
        }
    }
}

func validate(data []i64) !void {
    scope {
        for part in data.chunks(32) {
            spawn check(part, 1000)
        }
    }
}
```

If any task fails, `validate` fails. The scope waits for everybody first, and
when more than one task failed it reports the one spawned earliest — not the
one that happened to finish first, which would depend on how many threads ran
them.

### Nothing is cancelled

A failing task does not stop its siblings. They run to completion and then the
scope reports the error.

That is a choice, not an oversight. Cancellation means interrupting a task part
way through, possibly while it is in the middle of writing into memory borrowed
from the parent's arena — and making that safe is a much larger problem than it
looks. Sword would rather waste some work than hand you a half-written buffer.

### You cannot leave a scope early

```sword
scope {
    spawn work(a)
    if done {
        return 1      // compile error
    }
}
```

No `return`, no `break`, no `try` that escapes the block. The tasks are still
running on that stack frame; leaving would pull the floor out from under them.

In practice this means doing the fallible work before the scope rather than
inside it. It is the restriction you will bump into most, and it is the price
of the join being a brace rather than something you have to remember.

### `det`: the compiler says so

Everything above is about what cannot go wrong. `det` is how a function asks
to be told when it does:

```sword
det func total(xs []f64) f64 {
    mut sum := num.NewKahan()
    parallel for i in 0..xs.len reduce(num.Merge: sum) {
        sum.Add(xs[i])
    }
    return sum.Value()
}
```

The claim is that what it answers depends on its arguments and on nothing
else: not on the number of threads, not on the order they ran in, not on the
clock, and not on where anything sits in memory. The compiler checks it, and
it changes nothing about the code that comes out.

A `scope` full of tasks and a `parallel for` with a reduction are both allowed
inside one, which is the whole point of the rest of this chapter: two tasks
cannot reach the same memory, and a reduction combines its pieces in the order
they were cut. What is refused is an `atomic`, a `shared`, a call that leaves
the language, an address read as a number, and a call through an interface or
a function value that something not det implements.

A channel is refused unless it is used the way Kahn showed is safe: within
each scope, at most one of the things running at once sends on it (closing is
part of sending) and at most one receives, and nobody asks it anything but for
the next value. Then values arrive in the order one task sent them, whoever ran
when, and a pipeline is det:

```sword
det func pipeline(n i64, mut a mem.Allocator) !i64 {
    first := try chan[i64](a, 4)
    second := try chan[i64](a, 4)
    mut answer i64 = 0
    scope {
        spawn numbers(n, first)          // the only sender on first
        spawn squares(first, second)     // its only receiver, second's sender
        spawn fold(second, &answer)      // second's only receiver
    }
    return answer
}
```

A spawn in a loop is many tasks, and so is the body of a `parallel for`, so a
pool of workers on one channel is not det — which worker takes which value is
the scheduler's choice. `select`, `TrySend`, `TryRecv` and `Len` ask what only
the timing knows, and a channel copied into a second name, a field or a return
value is one the compiler can no longer follow; all of those take det away. A
function handed a channel answers from what arrives on it, the way one handed a
slice answers from what is in it; whether the channel has one sender is checked
where it is shared out.

Every function is analysed whether or not it says `det`; the word asks for the
answer. When it is lost, the message walks the chain:

```
error: 'doubled' is marked det, but what it answers can depend on more than
       its arguments
note: doubled calls mem.Alloc$i64, which is not det
note: mem.Alloc$i64 calls through an interface, and System.Alloc is not det
note: System.Alloc calls out of the language
```

That last one is worth reading twice. Allocating from an arena is det;
allocating from the system allocator is not, because the addresses it hands
back are the operating system's business. The same call is one or the other
depending on what the program binds into the interface, and the compiler knows
which because it can see the whole program.

## The race checker

One rule covers it:

> Within a `scope`, two accesses to the same memory are an error unless both
> only read, or they are pieces of one partition.

```sword
scope {
    spawn read(xs)
    spawn read(xs)      // fine: nobody writes
}

scope {
    spawn write(xs)
    spawn read(xs)      // error: this reads 'xs' while it is already written concurrently
}
```

Whether a task reads or writes comes from the parameter it is passed to. `mut`
on a parameter means the function may write through it; without `mut`, it
cannot. So the signature the task was declared with is what the checker reads,
and you do not annotate anything extra.

The same signature is what says a call writes. Inside a `parallel for`,
`c.Add(1)` and `bump(&c)` reach the same memory as `c.n += 1` would, and are
refused for the same reason — a write does not have to look like one.

### The parent counts too

```sword
scope {
    spawn read(xs)
    xs[0] = 9           // error
}
```

The join is the closing brace, so whatever the parent does inside the block
happens alongside the tasks. It is easy to forget that the parent is a
participant; the checker does not.

### What is actually shared

Copying a value is only safe when the copy owns everything it reaches.

| | |
|---|---|
| Scalars, `struct` of scalars, `[N]T`, `string` | copied outright, never tracked |
| `[]T` | the two-word header copies; the elements stay behind |
| `*T`, `[*]T`, an interface value | the pointee stays behind |
| A struct containing any of the above | tracked, through the field |

So this is caught, even though `Job` is passed by value:

```sword
struct Job {
    data []i64
}

func run(mut j Job) {
    j.data[0] = 1
}

// two tasks writing the same elements, through a struct
scope {
    spawn run(j)
    spawn run(j)        // error
}
```

A second name for the same memory is the same memory. A binding made from a
copy of another's slice or pointer — `ys := xs`, `p := &n`, a struct literal
holding `xs` — reaches what that other one reaches, and the checker compares
them as one:

```sword
mut p := &n
scope {
    spawn bump(p)
    n = 5               // error: this writes 'n' while it is already written
}
```

A binding reassigned keeps everything it was ever given. What a function hands
back is not followed: `ys := same(xs)` is a new name as far as this goes, so
keep copies that cross tasks in plain sight.

## Partitions

Handing different pieces of one slice to different tasks is safe, and it is the
central pattern of data parallelism. Sword does not try to prove your indices
do not overlap. It gives you the one construction that cannot produce
overlapping pieces:

```sword
for part in dst.chunks(8) {
    spawn fill(part)
}
```

Each `part` is a distinct piece, so they can all be written in parallel. The
last one is short when the length does not divide evenly. (`for x in xs` walks
the elements one at a time, which is the loop to reach for when there is no task
involved.)

Slicing by hand inside a loop is rejected:

```
error: every turn of this loop hands the same memory to a new task, so the
       tasks would overlap in 'dst'
note: walk it with 'for part in dst.chunks(n)' to give each task a piece of
      its own
```

The alternative would be inferring disjointness from a syntactic pattern like
`xs[i*k .. i*k+n]`. That works until you write the same thing slightly
differently, and then it rejects correct code for reasons that are hard to
explain. A guarantee by construction never fails quietly.

When you need the position as well as the piece — to index a second array in
step — bind both:

```sword
scope {
    for off, part in dst.chunks(8) {
        spawn square(src[off..off+part.len], part)
    }
}
```

`src` is read by every task, which is allowed. `part` is written by exactly
one.

A piece is its task's for one turn of the loop that cut it. A loop around the
`chunks` loop walks the slice again, a loop inside it hands the same piece out
again, and two spawns in one turn give it to two tasks; each is refused like
slicing by hand, with a note that points at the `chunks` loop. A spawn in each
branch of an `if` is fine, since only one of them starts.

## `parallel for`

For the common case of "do this to every element", there is a loop that spreads
itself:

```sword
parallel for i in 0..n {
    xs[i] = xs[i] * 2
}
```

This is the one construct in the language that captures. The compiler works out
which locals the body uses, hands the workers a block of pointers to them, and
that is safe because the loop is synchronous — the parent's frame is not going
anywhere while it runs.

Because iterations run on different workers, the body may only write what
exactly one of them can reach: `xs[i]`, with `i` the loop variable. And once
the loop writes a name, every other mention of it has to be indexed too:

```sword
parallel for i in 0..n {
    xs[i] = xs[i+1]     // error: 'xs' is written by this loop, so it can only be reached as 'xs[i]'
}
```

That is the neighbour bug, and it is the kind of thing that shows up in
production as a heisenbug three months later.

There is no `break` either. The iterations have no order, so there is no point
after which the rest would be skipped; `continue` skips the one index it is in.

### Reductions

Accumulating into one variable is the exception that needs help:

```sword
mut total i64 = 0
parallel for i in 0..n reduce(+: total) {
    total += xs[i]
}

mut biggest i64 = 0
parallel for i in 0..n reduce(max: biggest) {
    if xs[i] > biggest {
        biggest = xs[i]
    }
}
```

The range is cut into pieces; each piece accumulates into a private copy
started at the operator's identity, and the copies are combined once at the
end — so there is no contention per iteration. Without the `reduce` clause,
writing the variable in the body is an error, and the message tells you to add
it.

The body does the per-element combining; the clause only says how the pieces'
copies are joined. `+`, `&`, `|`, `min` and `max` are available, and `+` also
applies to floats.

A float sum is the one place where the order of the additions changes the
answer, so the language fixes it: each piece adds into sixteen separate sums
and folds them at the end, in an order the
[reference](reference.md#reductions) spells out. That is the order a vector
unit wants, which is why the same loop runs four to eleven times faster on one
core than adding one element after another (Apple M4, f64 and f32), and it is
the same order on any machine and any number of threads.

### A reduction of your own

The clause also takes the name of a function, which is what makes the rest of
this a property of the construct rather than of the five operators the
compiler happens to know:

```sword
mut total := num.NewKahan()
parallel for i in 0..xs.len reduce(num.Merge: total) {
    total.Add(xs[i])
}
answer := total.Value()
```

`num.Merge` takes two partial answers and gives a third — `func(T, T) T`,
writing through nothing, since the combining walks the pieces and must not
write into them. Each piece starts from the zero value of `T`, which is why
this suits an accumulator where all-zero means "nothing added yet"; `&` and
`min` keep their own identities and stay built in.

Everything above still holds, and holds for the same reason: the cut is fixed,
each piece answers in its own slot, and the combining walks them in the order
they were cut. The compiler does not need to know what the operation is.

### The answer does not depend on the threads

How the range is cut depends on the range and nothing else, each piece keeps
its partial answer in a slot of its own, and they are combined in the order the
pieces were cut. So a reduction answers the same thing on one thread and on
sixteen, and the same thing twice in a row.

For integers that was already true — addition of whole numbers does not care
about order. For floating point it was not: the pieces used to be folded in as
they finished, and the harmonic series over two million terms printed a
different number for each of six thread counts. Now it prints one.

What is *not* claimed is that it equals the sequential sum. The pieces are
summed separately and then combined, which is a different order of roundings
from adding the terms one after another. Reproducible is not the same as
sequential, and the difference is real for floating point.

Only the cut is fixed. How many tasks run it is the scheduler's business — with
few workers one task walks several pieces in a row — because who ran a piece
and when cannot change what that piece answers.

### Beyond a reduction

`std/par` holds the other operations that fit the same rule — a cut that
depends on the length, pieces that answer in their own place, and joining in
the order of the cut: a prefix sum, a filter that keeps the original order, the
position of the smallest element with ties going to the first, and a stable
sort. Each is det when the function it is given is, and a det function that
hands one something that is not is refused with the chain that says why.

```sword
det func ranks(xs []f64, mut out []f64) !void {
    try par.Scan[f64](xs, out, add)
}
```

### How much faster

A loop with real work in the body, 8192 iterations, on a ten-core machine:

| Threads | Time | Speedup |
|---|---|---|
| 1 | 0.507 s | 1.00× |
| 2 | 0.264 s | 1.92× |
| 4 | 0.156 s | 3.25× |
| 8 | 0.100 s | 5.07× |

`SWORD_THREADS` overrides the worker count, which defaults to one per core.
Useful for measuring, and for pinning down whether a bug is a race.

## The scheduler

The runtime is a few hundred lines and links as a static archive. Every program
carries the part that runs `main` as a task; a program that never spawns
anything pulls in nothing else, and starts no thread of its own.

Each worker has its own deque. The owner pushes and pops at one end, thieves
take from the other, and the victim rotates so workers do not all converge on
the same target. `parallel for` cuts the range into several chunks per worker —
enough slack for stealing to even out an uneven body, without paying task
overhead per iteration.

A thread waiting on a scope does not idle. It runs whatever work it can find
until the count reaches zero, which is what makes nested scopes safe from
deadlock: the thread blocked on the inner scope is also a thread available to
run its tasks.

Task arguments up to 96 bytes ride inside the task itself, and finished tasks
go back on a per-worker free list, so spawning in a loop does not touch the
allocator. Stacks come from the same kind of pile: mapping one costs far more
than keeping it.

A poller thread sits in kqueue or epoll and does nothing else. It owns no work;
it moves tasks the kernel has declared ready back onto the run queues.

## Waiting

**A task has its own stack.** That is what lets it stop in the middle of
something and be picked up later, and it is the whole reason a socket does not
cost a thread.

When a task reads from a socket with nothing to say, the runtime puts the task
down: it registers the descriptor with a poller — kqueue or epoll — switches
back to the worker's own stack, and the worker goes to whoever does have data.
When the kernel says the descriptor is ready, the task goes back on a run queue
and whichever worker gets to it first resumes it, on the line after the read.

Nothing in your program says any of this. `try c.Read(buf)` is the whole of it:

```sword
func handle(c net.Conn) !void {
    mut buf := [1024]u8{}
    n := try c.Read(buf[..])   // the task stops here; the thread does not
    try c.Write(buf[0..n])
}
```

The same is true of a TLS handshake, which is several round trips rather than one:
OpenSSL says it needs more bytes, the task goes down, the poller brings it back.
Twenty clients arriving at once are twenty tasks on however many threads you have,
not twenty threads waiting on twenty handshakes.

### What a stack costs

A stack is reserved rather than committed, so what a waiting task actually costs
is the few pages it has touched. Measured on a real server: a keep-alive
connection sitting idle costs about 33 KiB, against roughly 80 KiB and a whole
thread before. Two thousand of them run on twelve threads.

A megabyte is the default reservation, and `SWORD_STACK_KB` changes it — down for
a server whose handlers are shallow and which wants a great many of them, up for
code that recurses or keeps large buffers in frames. `runtime.Read().StackBytes`
reports what it settled on.

Stacks come thirty-two to a mapping, and a stack that nobody wants goes back on a
pile with its pages dropped rather than kept. That is what stops a spike from
costing memory for the rest of the run: twenty thousand tasks each touching a
16 KiB buffer sit in 338 MiB, and when they finish the memory goes back.

### Running out of it

Below every task's stack is a guard, and a write into it stops the program with
what happened:

```
sword: a task ran out of stack. It had 1024 KiB; SWORD_STACK_KB sets that.
```

The guard is 64 KiB rather than a single page, because a frame that reserves a
large buffer and writes into the middle of it can step over one page without
touching it — and with stacks packed together, what is on the other side belongs
to another task.

This covers `main` as well, since it runs on a task's stack like everything
else. A fault anywhere else is left alone: a wild pointer dies the way it
always did.

### What cannot be put down

Not everything is a socket. A file is never "not ready yet" — the wait is the
disk, and asking a poller about a regular file tells you nothing. Name resolution
is one call in the C library with no way into the middle of it.

Those go to a pool of threads kept for the purpose. The task waiting on one is
put down like any other, the worker goes on to something else, and the threads
doing the waiting are not scheduler capacity and never run Sword code. What forty
concurrent file reads cost is the size of that pool — ten threads on a ten-core
machine — and not forty threads, which is what it used to be.

The pool grows only while every thread in it is busy, gives a thread back after a
second of finding nothing to do, and stops at `SWORD_IO_THREADS`. Past that,
calls queue: a server whose disk is the bottleneck should wait on the disk rather
than on a thread it cannot afford.

`runtime.Read().IoThreads` reports how many are up.

### The fallback underneath

A thread can still say it is about to block:

```
sword_blocking_enter();
something_that_stops_the_thread();
sword_blocking_exit();
```

A parked thread stops counting as scheduler capacity, so the pool hires a
replacement while it is gone and retires it after 200 ms of finding no work. It is
what code outside any task falls back to — the main function, before any `scope` —
and it is why a program that blocks somewhere unforeseen slows down instead of
stopping.

Code that is not in a task at all — the main function, before any `scope` — has
nothing to put down, so it waits on the thread the ordinary way.

Three numbers control the shape of it:

| Variable | Meaning |
|---|---|
| `SWORD_THREADS` | Workers. Defaults to one per core. |
| `SWORD_MAX_THREADS` | How large the pool may grow to cover threads stuck in the kernel. Defaults to 512. |
| `SWORD_STACK_KB` | How much stack a task gets. Defaults to 1024, held between 64 and 262144. |
| `SWORD_IO_THREADS` | Threads for calls that cannot be put down — file I/O, name resolution. Defaults to the core count, at least 4. |

The ceiling is a brake rather than a wall. A pool at its limit with every thread
parked and work still queued would be a program that has stopped, so in that one
case the runtime goes over the limit instead.

### Sleeping

`time.Sleep` goes the same way. A hundred tasks waiting a second are a hundred
timers on the poller, not a hundred threads:

```
40 tasks sleeping     11 threads
```

Before it went through the poller the same program ran 50.

### Watching it run

`std/runtime` reports what the scheduler is doing — threads, tasks queued, tasks
running, stacks alive — for a few relaxed loads. `Queued` is the one worth an
alert: a queue that only grows means the program is taking on more than it
finishes.

### Deadlines

A timeout bounds one wait. It cannot bound a client that sends a byte every
twenty milliseconds forever, because no single wait ever runs out. That is what
a deadline is for:

```sword
try c.SetTimeout(time.Seconds(5))                      // any one wait
try c.SetDeadline(time.Now().Add(time.Seconds(30)))    // the whole exchange
```

Past the deadline every read and write on that connection answers
`error.Timeout`, and the handler leaves through the `try` it was already
written with. Nothing is interrupted: a deadline is collected where the task was
going to stop anyway, which is the only place it can be collected without
tearing a task in half.

## Shared counters

`reduce` covers accumulating over a loop. For anything else — a hit counter, a
flag, a sequence number — there is `atomic[T]`:

```sword
mut hits := atomic[u64](0)

scope {
    for part in rows.chunks(64) {
        spawn count(part, &hits)
    }
}
total := hits.Load()
```

An atomic is the one exemption the race checker makes: several tasks may write
one at the same time. Everything reached through it goes through `Load`,
`Store`, `Add`, `Sub`, `And`, `Or`, `Swap` and `CompareSwap`, each a single
instruction with sequentially consistent ordering. There is no way to reach the
raw value, which is what makes the exemption safe to grant.

A plain integer gets no such treatment:

```sword
mut counter u64 = 0
scope {
    spawn bump(&counter)
    spawn bump(&counter)   // error: this writes 'counter' while it is already written concurrently
}
```

Each operation on an atomic is indivisible; two of them in a row are not.

```sword
func bump(counter *atomic[u64]) {
    seen := counter.Load()
    counter.Store(seen + 1)     // compiles, and loses updates
}
```

Two tasks can both load before either stores, and one increment disappears.
The race checker has nothing to say — every access went through the atomic,
which is exactly what it asks — because this is not a data race but a race in
the logic. `Add` is the one-step version. For the cases with no one-step
version, and to find the ones you did not see, `shield test -sim` runs a test
under many orders on purpose and prints the one that breaks it;
[Testing](testing.md#simulation) shows it finding this very function.

## Shared structures

An atomic covers one scalar. A hit table, a work queue, a cache — anything with
more than one field in it — needs something else, because a counter's worth of
synchronisation cannot cover a structure whose invariants span several writes.

That is `shared[T]`:

```sword
import "std/collections"
import "std/mem"

func count(mut table *shared[collections.Map[string, u64]], words []string) !void {
    for i in 0..words.len {
        lock m := table {
            seen := m.Get(words[i]) orelse 0
            try m.Set(words[i], seen + 1)
        }
    }
}

func main() !int {
    mut backing := [131072]u8{}
    mut arena := mem.NewArena(backing[..])

    mut table := shared[collections.Map[string, u64]](
        try collections.NewMap[u64](&arena, 64))

    mut words := [2]string{}
    words[0] = "ada"
    words[1] = "grace"

    scope {
        for i in 0..6 {
            spawn count(&table, words[..])
        }
    }

    lock m := &table {
        return int(m.Len())
    }
}
```
<!-- exits: 2 -->

Six tasks writing one map, and it compiles. The exemption the race checker makes
is the same one it makes for an atomic, and it rests on the same thing: **there
is no way to reach the value except through the lock.** `shared[T](v)` builds
one, `lock` opens one, and the type has no fields you can read.

### The block is the critical section

```sword
lock m := table {
    ...
}
```

The opening brace takes the mutex and the closing brace releases it. So does a
`return` out of the middle, a `break`, a `continue`, or a `try` that fails —
it unwinds the same way `defer` does, and for the same reason. There is no
`Unlock` to forget and no way to unbalance the pair.

Inside, the name is a mutable `*T`. Outside, it does not exist.

Nothing here is marked `mut`, and that is on purpose. The type already says the
value will be written by whoever holds the lock, so the binding's mutability has
nothing left to decide — exactly as with an atomic. A shared can also sit in a
struct field, and the struct around it stays an ordinary struct:

```sword
struct Server {
    name  string
    tally shared[Tally]
}

func serve(s *Server) {
    lock c := &s.tally {
        c.hits += 1
    }
}
```

### Two rules

**A lock inside a lock on the same value is an error.**

```sword
lock a := &table {
    lock b := &table {      // error: 'table' is already locked here
    }
}
```

That is the version the compiler can see. The version it cannot — a second
`lock` reached through a function call — aborts at runtime with a message rather
than hanging, because the guard remembers which task holds it.

**A task cannot outlive the lock it starts under.**

```sword
scope {
    lock c := &table {
        spawn touch(c)      // error: this task would outlive the 'lock'
    }
}
```

The lock releases at its own brace, the scope joins at its own, and the task is
still running in between. Put the `scope` inside the `lock` — which is safe and
useful, parallel work over a locked structure — or put the `lock` inside the
task.

What neither rule covers is two different values locked in opposite orders:
one task holds `a` and wants `b`, the other holds `b` and wants `a`. Nothing
about either task is wrong on its own, so the compiler accepts both, and whether
they deadlock depends on how they interleave. Take locks in one order everywhere.
To check that you did, `shield test -sim` tries many interleavings and reports
one that deadlocks, with the seed that makes it happen again.

### What it costs

A contended lock spins for a moment and then puts the task down behind it, and
the thread goes on to other work until the lock is handed over. So a task
waiting behind a long critical section does not cost a core, and a task that
holds a lock while it waits — for a timer, a socket, a `Wait` — lets the others
behind it wait without taking a thread each. Eight tasks doing 200 000 locked
increments each — 1.6 million lock/unlock pairs — take about 22 ms on a
ten-core machine. The same work through one `atomic[u64]` takes about 35 ms,
because eight threads hammering one cache line contend harder than eight
threads taking turns. Read nothing general
into that: the point is only that the lock is not going to be your bottleneck.

### Readers, and not waiting

A value that is read far more often than it is written — a configuration, a
routing table, a cache that misses rarely — does not need its readers to take
turns. `lock read` lets any number in together and keeps writers out while they
are there; the binding is an immutable `*T`, so a reader cannot write by
mistake, and `Wait` and `Notify` need the writer's `lock`:

```sword
lock read routes := &table {
    target := routes.Get(path) orelse return error.NotFound
}
```

A writer waiting keeps new readers out, so a steady stream of them cannot keep
it waiting for good.

And when waiting is the wrong answer — a cache that is busy can be skipped, a
statistic can be counted next time — `if lock` takes the lock only if it is
free, and runs `else` without it otherwise:

```sword
if lock stats := &counters {
    stats.hits += pending
    pending = 0
}
```

## Channels

A queue tasks hand values through. A send waits while it is full, a receive waits
while it is empty, and waiting costs a stack rather than a thread:

```sword
import "std/mem"

func worker(jobs chan[u64], mut done *atomic[u64]) !void {
    for job := <-jobs {
        done.Add(job)
    }
}

func main() !int {
    mut backing := [65536]u8{}
    mut arena := mem.NewArena(backing[..])
    mut jobs := try chan[u64](&arena, 8)
    mut done := atomic[u64](0)

    scope {
        for w in 0..4 {
            spawn worker(jobs, &done)
        }
        spawn fill(jobs)
    }
    return int(done.Load() % 100)
}

func fill(jobs chan[u64]) !void {
    for i in 0..100 {
        try jobs <- u64(i)
    }
    close(jobs)
}
```
<!-- exits: 50 -->

Four consumers on one channel, and every value reaches exactly one of them.
`for job := <-jobs` runs while there is something there — closing the channel is
what ends all four loops.

Three forms and nothing else to learn: `jobs <- v` sends, `<-jobs` receives, and
`close(jobs)` says there will be no more. A receive answers `?T`, which is how a
closed channel is told apart from a value without a second return or a flag — and
why `for job := <-jobs` is the loop rather than a special one.

A send can fail, which is why it takes a `try`: sending into a closed channel is a
mistake rather than a wait. A receive cannot fail — there is nothing to fail at —
so it takes nothing.

**A channel is a handle.** Copying one copies the handle and both copies are the
same channel, so it goes to a task by value and no pointer is involved. The memory
behind it comes from the allocator you pass, like everything else here; there is
no `make`, because nothing in this language allocates without being asked.

**Capacity zero is a handover.** Not a queue with no room — a meeting point: the
sender waits until a receiver has actually taken the value:

```sword
mut hand := try chan[u64](&arena, 0)
```

That is the one to reach for when the point is that the two tasks are at the same
place at the same time, rather than that a value got queued. On one of these
`TrySend` answers false unless a receiver is already waiting, because the
alternative would be exactly the wait it promises not to do.

**Closing is the end of the stream.** What is already in the channel is still
received; after that every receive answers nil, and a send fails rather than
waiting for a reader who will never come. Closing twice is harmless, because
whoever closes is often not whoever knows.

**The capacity is the backpressure.** A channel of one slot against a consumer
that takes five milliseconds per value holds the producer to the consumer's pace
— which is the point, and the alternative is an unbounded queue that eventually
eats the machine. `TrySend` and `TryRecv` are there for when waiting is not
wanted.

### Waiting on several at once

A server does two things at the same time: take work, and notice when somebody
says stop. `select` waits until one of its cases can go, and runs that one:

```sword
func work(jobs chan[u64], quit chan[u64]) !void {
    for {
        select {
        case job := <-jobs:
            j := job orelse return      // the channel closed: no more work
            try handle(j)
        case <-quit:
            return
        case idle <- 1:                 // a send case: when there is room
            continue
        }
    }
}
```

A **receive** case binds `?T`, the same thing `<-ch` gives you, because a closed
channel is an answer and the body has to be able to see it. That is what `orelse
return` is doing above, and it is the line to remember: a closed channel makes its
case ready *every time*, so a loop that ignores the nil spins instead of ending.

A **send** case goes when there is room. If its channel is closed it can never go,
and is skipped — a select that has nothing left but closed send channels stops the
program and says so, which is better than waiting for something that cannot happen.

**`default` never waits.** With one, a select that finds nothing ready runs the
default instead; without one, it waits. That is the whole difference between
"check" and "wait".

Cases are tried from a **rotating start**, so a channel that always has something
cannot starve the others. Waiting costs a stack rather than a thread here too: the
task registers on every channel in the select and is put down until any one of them
moves.

### How it is built

A channel's state lives in the runtime, and the type-aware part — `chan[T]`, the
allocation, the values going in and out — is written in Sword, in a package the
compiler imports when a program mentions `chan`. Nobody writes that import.

The split is where it is because of `select`: waiting on several channels means
holding all of them and registering a waiter on each, and `lock` is lexical while
the number of cases is not. Everything else could have stayed in Sword, and did,
for as long as there was no `select`.

### Wait and Notify

Those two are worth knowing on their own, because a channel is not the only
queue anyone ever wants. Both are called with the lock held; `Wait` lets go of
it, puts the task down, and takes it back before returning:

```sword
lock r := &box {
    for r.count == 0 {
        box.Wait()          // the lock is somebody else's while this waits
    }
    take(r)
}
```

A caller loops rather than testing once, because what it was waiting for may
have been taken again by the time it wakes. `Notify` wakes one waiter and
`NotifyAll` wakes all of them.

## What is not here yet

**Memory with no name behind it.** The checker compares accesses by the binding
they come from. A slice returned straight out of a call, never bound to
anything, is not tracked — there is nothing to compare it against.

**Growable stacks.** A task's stack is one size for its whole life. Overflowing
it is now reported rather than mysterious, and `SWORD_STACK_KB` is there for code
that needs more, but nothing grows on demand. Go moves stacks to grow them, which
works because its compiler can find every pointer into a stack and rewrite it;
Sword hands out `&local` and slices of stack arrays freely, and there is no way
to find them all after the fact.

What that leaves is a ceiling on live tasks. Each stack needs its guard, the
kernel counts a protected range as a mapping of its own, and Linux allows around
sixty-five thousand — so tens of thousands of tasks at once is the honest number,
and a machine that needs more can raise `vm.max_map_count`. Packing stacks into
slabs made them cheaper to hand out; it did not move that limit.
