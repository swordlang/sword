# A parallel sum

The same job done two ways: by hand, with tasks and a partition of the results,
and with a `parallel for` that cuts the range itself. The second prints the
same bits however many threads run it.

Uses: [tasks](../tour.md#tasks), [memory](../tour.md#memory),
[partitions](../concurrency.md#partitions),
[`parallel for`](../concurrency.md#parallel-for),
[reductions](../reference.md#reductions).

```sword
import "std/io"
import "std/mem"

const Parts = 8

// One task's share: its piece of the input, and the one slot it answers in.
func sum(xs []u64, mut slot []u64) {
    mut total u64 = 0
    for x in xs {
        total += x
    }
    slot[0] = total
}

func min(a u64, b u64) u64 {
    if a < b {
        return a
    }
    return b
}

func harmonic(n u64) f64 {
    mut total f64 = 0.0
    parallel for i in 1..n + 1 reduce(+: total) {
        total += 1.0 / f64(i)
    }
    return total
}

func main() !void {
    mut backing := [131072]u8{}
    mut arena := mem.NewArena(backing[..])
    mut xs := mem.Alloc[u64](&arena, 10000) orelse return error.OutOfMemory
    for i in 0..xs.len {
        xs[i] = i + 1
    }

    // By hand: a partition of the results, one slot per task, each task
    // reading its own stretch of the input.
    mut partials := mem.Alloc[u64](&arena, Parts) orelse return error.OutOfMemory
    size := (xs.len + Parts - 1) / Parts
    scope {
        for k, slot in partials.chunks(1) {
            start := k * size
            spawn sum(xs[start..min(start + size, xs.len)], slot)
        }
    }
    mut total u64 = 0
    for p in partials {
        total += p
    }
    try io.Printf("1 + 2 + ... + {} = {}\n", xs.len, total)

    // With a reduction: the loop cuts the range itself.
    try io.Printf("1 + 1/2 + ... + 1/2000000 = {}\n", harmonic(2000000))

    // One addition after another, for comparison.
    mut plain f64 = 0.0
    for i in 1..u64(2000001) {
        plain += 1.0 / f64(i)
    }
    try io.Printf("the same, in order        = {}\n", plain)
}
```
<!-- prints:
1 + 2 + ... + 10000 = 50005000
1 + 1/2 + ... + 1/2000000 = 15.085873653425745
the same, in order        = 15.085873653425047
-->

## By hand

```sword
for k, slot in partials.chunks(1) {
    start := k * size
    spawn sum(xs[start..min(start + size, xs.len)], slot)
}
```

Eight tasks, each adding up an eighth of `xs` and writing its answer into a slot
of its own. Every task reads `xs`, which is allowed, because nobody writes it.
What they write is `partials`, and the only way to give each task a piece of it
to write is a partition: `partials.chunks(1)` hands out one element at a time,
and the race checker knows those pieces cannot overlap. `k` is where the piece
starts, which for pieces of one is also the task's number.

Writing `partials[k..k + 1]` instead is refused. The checker does not try to
prove that two expressions never meet; it accepts the one construction that
cannot produce overlapping pieces. `chunks` needs a slice, which is why
`partials` comes from the arena rather than being an array on the stack.

The tasks are joined at the closing brace of `scope`, so by the next line every
slot is filled, and adding the eight of them up is ordinary code.

## With a reduction

```sword
parallel for i in 1..n + 1 reduce(+: total) {
    total += 1.0 / f64(i)
}
```

The loop cuts the range into pieces, gives each a private `total`, and adds the
pieces' answers together at the end. Without `reduce(+: total)` the body could
not write `total` at all: every iteration would be writing the same variable.

## The same bits, every time

How the range is cut depends on the range alone, and the pieces are combined in
the order they were cut. So the harmonic sum prints the same digits on one
thread and on sixteen. Run it with `SWORD_THREADS=1` and with `SWORD_THREADS=16`
and the output is the same, byte for byte.

It is not the number the loop after it prints, which adds one term after
another. Floating-point addition rounds at every step, and a different order is
a different set of roundings. Each piece of a float reduction keeps sixteen
running sums and folds them at the end, which is the order a vector unit wants.
Here that order also lands closer: the exact sum of these two million terms,
rounded once, is 15.08587365342573, and the reduction first differs from it in the
fourteenth decimal place, the plain loop in the thirteenth.

Reproducible is what the language promises, not sequential. A program that
needs the sequential answer writes the sequential loop.
