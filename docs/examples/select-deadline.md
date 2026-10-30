# Waiting with a deadline

Two questions asked with a time limit: one answered at once, one that cannot be
answered before the limit runs out. A `select` waits for whichever comes
first, and a closed channel is the deadline.

Uses: [tasks](../tour.md#tasks), [channels](../tour.md#channels),
[pointers and optionals](../tour.md#pointers-and-optionals),
[waiting on several at once](../concurrency.md#waiting-on-several-at-once),
[`std/time`](../reference.md#stdtime).

```sword
import "std/io"
import "std/mem"
import "std/time"

// Closes the channel once the time is up. A closed channel is ready in every
// select that waits on it, so this one channel can stop any number of them.
func timer(after time.Duration, done chan[u64]) {
    time.Sleep(after)
    close(done)
}

// A lookup that answers at once.
func quick(answer chan[u64]) !void {
    try answer <- 42
}

// A lookup that cannot answer until it is let go, which here is only after
// whoever asked has stopped waiting.
func stuck(answer chan[u64], release chan[u64]) !void {
    for v := <-release {
    }
    answer.TrySend(7)
}

// Waits for an answer or the end of the time, whichever comes first, and
// answers nil for the second.
func ask(answer chan[u64], done chan[u64]) ?u64 {
    select {
    case v := <-answer:
        return v
    case <-done:
    }
    return nil
}

func show(name string, got ?u64) !void {
    if n := got {
        try io.Printf("{}: {}\n", name, n)
    } else {
        try io.Printf("{}: no answer in time\n", name)
    }
}

func main() !void {
    mut backing := [16384]u8{}
    mut arena := mem.NewArena(backing[..])

    first := try chan[u64](&arena, 1)
    firstDone := try chan[u64](&arena, 0)
    mut got ?u64 = nil
    scope {
        spawn timer(time.Millis(500), firstDone)
        spawn quick(first)
        got = ask(first, firstDone)
    }
    try show("quick", got)

    second := try chan[u64](&arena, 1)
    secondDone := try chan[u64](&arena, 0)
    release := try chan[u64](&arena, 0)
    scope {
        spawn timer(time.Millis(50), secondDone)
        spawn stuck(second, release)
        got = ask(second, secondDone)
        close(release)
    }
    try show("stuck", got)
}
```
<!-- prints:
quick: 42
stuck: no answer in time
-->

## A timer is a task

```sword
func timer(after time.Duration, done chan[u64]) {
    time.Sleep(after)
    close(done)
}
```

There is no timer type. A task that sleeps and then closes a channel is one,
and `time.Sleep` puts the task down rather than its thread, so a thousand of
them waiting cost a thousand small stacks and no threads.

It closes the channel rather than sending on it. A send wakes one receiver; a
closed channel is ready for every receive from then on, so one `done` can end
any number of waits, and a wait that starts after the time is up ends at once.

## Whichever comes first

```sword
select {
case v := <-answer:
    return v
case <-done:
}
return nil
```

`select` waits until one of its cases can go and runs that one. The first case
goes when an answer arrives, and `v` is a `?u64`: the value, or nil if
`answer` was closed. The second goes when `done` closes. `ask` hands back the
answer, or nil when the time ran out first.

The quick lookup has its answer in the channel long before half a second is
up. The stuck one waits on `release`, a channel nobody sends on, which `main`
closes only after `ask` has given up. So the order of events in this program
is fixed, and so is what it prints.

## Nothing is cancelled

The `scope` still waits for every task it started. When `ask` gives up on the
stuck lookup, the lookup is still there, and the closing brace waits for it.
That is why `main` closes `release` before the brace: without it, the program
would wait for an answer it had already stopped wanting, forever.

Sword does not stop a task from outside, because a task stopped at an
arbitrary point leaves whatever it was in the middle of half done. A task that
should stop early is told so through a channel it listens on, as `stuck` is
here, and decides for itself where to stop. Deadlines on network connections
work the same way: they are collected where the task was going to wait anyway,
at a read or a write (see [deadlines](../concurrency.md#deadlines)).

The first timer sleeps its half second even though the answer came at once,
for the same reason, and the program takes that long to finish. A timer that
should end early listens on a channel too.
