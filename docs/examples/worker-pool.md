# A worker pool

A pipeline of three stages joined by channels: one task produces numbers, four
work on them, one collects the answers. The work is counting Collatz steps for
every number up to 100 000, and the answer is the number that takes longest.

Uses: [tasks](../tour.md#tasks), [channels](../tour.md#channels),
[pointers and optionals](../tour.md#pointers-and-optionals),
[channels in depth](../concurrency.md#channels).

```sword
import "std/io"
import "std/mem"

struct Result {
    n     u64
    steps u64
}

// How many steps of the Collatz rule take n down to 1: halve it when even,
// triple it and add one when odd.
func collatz(n u64) u64 {
    mut x := n
    mut steps u64 = 0
    for x != 1 {
        if x % 2 == 0 {
            x /= 2
        } else {
            x = 3 * x + 1
        }
        steps += 1
    }
    return steps
}

// The first stage: the numbers to work on, then the end of them.
func produce(jobs chan[u64], count u64) !void {
    for n in 1..count + 1 {
        try jobs <- n
    }
    close(jobs)
}

// The middle stage, run by several tasks at once on the same two channels.
func work(jobs chan[u64], results chan[Result]) !void {
    for n := <-jobs {
        try results <- Result{n: n, steps: collatz(n)}
    }
}

// The last stage keeps the longest. The order results arrive in depends on
// the workers; the answer does not, since ties go to the smaller number.
func collect(results chan[Result], mut best *Result, mut seen *u64) {
    for r := <-results {
        *seen += 1
        if r.steps > best.steps || r.steps == best.steps && r.n < best.n {
            *best = r
        }
    }
}

func main() !void {
    mut backing := [65536]u8{}
    mut arena := mem.NewArena(backing[..])
    mut jobs := try chan[u64](&arena, 64)
    mut results := try chan[Result](&arena, 64)
    mut best := Result{n: 0, steps: 0}
    mut seen u64 = 0

    scope {
        spawn produce(jobs, 100000)
        spawn collect(results, &best, &seen)
        scope {
            for w in 0..4 {
                spawn work(jobs, results)
            }
        }
        // Every worker has finished, so nothing more will be sent.
        close(results)
    }
    try io.Printf("{} numbers; {} takes the most steps, {}\n", seen, best.n, best.steps)
}
```
<!-- prints:
100000 numbers; 77031 takes the most steps, 350
-->

## The stages

`produce` sends the numbers and then closes `jobs`. Closing says there will be
no more: what is already in the channel is still received, and after that
every receive answers nil.

`work` runs four times over, and all four receive from the same `jobs`. Each
value reaches exactly one of them. `for n := <-jobs` is a loop over a channel:
it runs while a receive answers a value and stops at the first nil, so closing
`jobs` is what ends all four.

`collect` keeps the best result through a pointer into `main`'s frame,
`mut best *Result`. Lending it is safe because the task cannot outlive the
`scope` it was spawned in, and `main`'s frame outlives the `scope`. Nothing
else touches `best` while the scope is open, and the race checker holds the
program to that: a second task given `&best`, or `main` reading it before the
closing brace, would not compile.

## Closing the second channel

```sword
scope {
    for w in 0..4 {
        spawn work(jobs, results)
    }
}
close(results)
```

`results` has four senders, and none of them knows when the others are done,
so none of them can close it. The inner `scope` knows: its closing brace waits
for all four workers, and the line after it is the one moment every sender has
finished. That is where `results` is closed, and closing it is what ends
`collect`'s loop.

The capacity of each channel, 64, is how far a stage may run ahead of the next.
A full channel makes the sender wait, so a producer faster than the workers is
held to their pace instead of filling memory.

## An answer that does not depend on the order

Results arrive in whatever order the workers finish, and that order changes
from one run to the next. The answer does not, because `collect` breaks ties by
the smaller number: of two numbers with the same count of steps, the same one
wins whichever arrives first. Up to 100 000 only 77031 takes 350 steps, so the
tie never comes up here, but over another range it can. A pipeline that kept
the first of the longest to arrive would then print a different number now and
then, and nothing in the program would say why.

A channel is a handle. Passing `jobs` to five tasks copies the handle five
times, and all five copies are the same channel, which is why the parameters
are `chan[u64]` and not pointers.
