# Counters: atomic and shared

Sixteen tasks keeping two totals, a count of requests and the bytes they
carried. Eight keep them in two `atomic[u64]`, eight in one `shared[Totals]`,
and both reach the same numbers. Where the two differ is in what a reader can
see along the way.

Uses: [tasks](../tour.md#tasks), [structs](../tour.md#structs),
[shared counters](../concurrency.md#shared-counters),
[shared structures](../concurrency.md#shared-structures),
[atomics](../reference.md#atomics), [shared values](../reference.md#shared-values).

```sword
import "std/io"

// Two numbers that have to move together: the count of requests and the
// bytes they carried. No single instruction updates both.
struct Totals {
    requests u64
    bytes    u64
}

func serveAtomic(size u64, mut requests *atomic[u64], mut bytes *atomic[u64]) {
    for i in 0..1000 {
        requests.Add(1)
        bytes.Add(size)
    }
}

func serveShared(size u64, totals *shared[Totals]) {
    for i in 0..1000 {
        lock t := totals {
            t.requests += 1
            t.bytes += size
        }
    }
}

func main() !void {
    mut requests := atomic[u64](0)
    mut bytes := atomic[u64](0)
    totals := shared[Totals](Totals{requests: 0, bytes: 0})

    scope {
        for task in 1..u64(9) {
            spawn serveAtomic(task * 100, &requests, &bytes)
            spawn serveShared(task * 100, &totals)
        }
    }

    try io.Printf("atomic: {} requests, {} bytes\n", requests.Load(), bytes.Load())
    lock read t := &totals {
        try io.Printf("shared: {} requests, {} bytes\n", t.requests, t.bytes)
    }
}
```
<!-- prints:
atomic: 8000 requests, 3600000 bytes
shared: 8000 requests, 3600000 bytes
-->

## Why a plain counter does not compile

Give every task `&count` for an ordinary `mut count u64` and the program is
refused:

```
error: every turn of this loop hands the same memory to a new task, so the tasks would overlap in 'count'
```

`count += 1` is a read and a write, and two tasks doing it at once can both
read 41 and both write 42. In C that loses counts now and then and nothing
says so. In Sword it does not compile, and the two ways out are the two types
in this program.

## `atomic[u64]`

```sword
requests.Add(1)
bytes.Add(size)
```

An atomic is the one kind of memory several tasks may write at the same time.
Each `Add` is a single instruction that reads, adds and writes with no gap
between, so no count is lost. It is the cheapest way to share a number.

It covers one number. `requests` and `bytes` are each correct on their own,
but between the two `Add`s a reader could see a request counted whose bytes are
not yet: the pair is not updated as one.

## `shared[Totals]`

```sword
lock t := totals {
    t.requests += 1
    t.bytes += size
}
```

A shared value puts a lock in front of a whole structure. `lock` is the only
way in, the block is the time the lock is held, and inside it `t` is an
ordinary `*Totals`, written with ordinary code. Whoever takes the lock next
sees both fields changed or neither.

The binding needs no `mut`, and neither does the parameter: the type already
says it will be written by whoever holds the lock. At the end, `lock read`
takes it only to read, which lets any number of readers in at once.

## Which to use

An atomic for one number that stands alone: a counter of hits, a flag, the next
free id. A shared value for anything that has to stay consistent with itself:
two numbers that move together, a map, a queue. Taking a lock costs more than
an atomic add, and a task that finds it held spins briefly and is then put
down, so its thread goes on to other work until the lock is free.
