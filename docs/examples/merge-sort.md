# Merge sort

A merge sort written once for any element type, with its scratch space handed
in rather than allocated behind the caller's back. Then the same sort in
parallel: two tasks sort the halves, and the parent merges them.

Uses: [generics](../tour.md#generics), [function values](../tour.md#function-values),
[memory](../tour.md#memory), [tasks](../tour.md#tasks),
[partitions](../concurrency.md#partitions).

```sword
import "std/io"
import "std/mem"

// Sorts xs, using scratch, which must be as long, as room to merge into.
func mergeSort[T](mut xs []T, mut scratch []T, less func(T, T) bool) {
    if xs.len < 2 {
        return
    }
    mid := xs.len / 2
    mergeSort(xs[0..mid], scratch[0..mid], less)
    mergeSort(xs[mid..xs.len], scratch[mid..xs.len], less)
    merge(xs, mid, scratch, less)
}

// xs[0..mid] and xs[mid..] are each sorted; merges them through scratch.
// Taking from the left on a tie keeps equal elements in their order.
func merge[T](mut xs []T, mid u64, mut scratch []T, less func(T, T) bool) {
    mut i u64 = 0
    mut j := mid
    mut k u64 = 0
    for i < mid && j < xs.len {
        if less(xs[j], xs[i]) {
            scratch[k] = xs[j]
            j += 1
        } else {
            scratch[k] = xs[i]
            i += 1
        }
        k += 1
    }
    for i < mid {
        scratch[k] = xs[i]
        i += 1
        k += 1
    }
    for j < xs.len {
        scratch[k] = xs[j]
        j += 1
        k += 1
    }
    for n in 0..xs.len {
        xs[n] = scratch[n]
    }
}

// What one task does with its half: it makes its own allocator, because an
// allocator cannot be shared between tasks.
func sortHalf(mut half []i64) !void {
    mut sys := mem.NewSystem()
    mut scratch := mem.Alloc[i64](&sys, half.len) orelse return error.OutOfMemory
    mergeSort(half, scratch, before)
    mem.Free(&sys, scratch)
}

func before(a i64, b i64) bool {
    return a < b
}

func longer(a string, b string) bool {
    return a.len > b.len
}

func main() !void {
    mut backing := [65536]u8{}
    mut arena := mem.NewArena(backing[..])

    // Generic: the same function sorts strings by length.
    mut words := [6]string{"sword", "a", "compiled", "systems", "language", "is"}
    mut room := mem.Alloc[string](&arena, words.len) orelse return error.OutOfMemory
    mergeSort(words[..], room, longer)
    try io.Printf("{} {} {} {} {} {}\n", words[0], words[1], words[2], words[3], words[4], words[5])

    // In parallel: the two halves sorted by two tasks, then merged here.
    n u64 = 1000
    mut xs := mem.Alloc[i64](&arena, n) orelse return error.OutOfMemory
    for i in 0..n {
        xs[i] = i64(i * 7919 % n)
    }
    mid := (n + 1) / 2
    scope {
        for half in xs.chunks(mid) {
            spawn sortHalf(half)
        }
    }
    mut scratch := mem.Alloc[i64](&arena, n) orelse return error.OutOfMemory
    merge(xs, mid, scratch, before)

    mut sorted := true
    for i in 1..n {
        if xs[i - 1] > xs[i] {
            sorted = false
        }
    }
    try io.Printf("{} numbers, sorted: {}, from {} to {}\n", n, sorted, xs[0], xs[n - 1])
}
```
<!-- prints:
compiled language systems sword is a
1000 numbers, sorted: true, from 0 to 999
-->

## Generic over the element

```sword
func mergeSort[T](mut xs []T, mut scratch []T, less func(T, T) bool)
```

`T` is whatever the slice holds, and `less` says what order means for it. The
same function sorts strings by length and integers by value; each use is
compiled for its own type, and `less` is a plain function value, called
through a pointer.

The sort needs as much room again as it sorts, to merge into, and it does not
get that room for itself. Nothing in Sword allocates without being handed an
allocator, and a sort that took one would be deciding something that belongs
to the caller: whether the room comes from an arena, from the heap, or from a
buffer that is reused across a thousand sorts. Taking `scratch` as a parameter
leaves all of that outside.

The recursion splits `xs` and `scratch` at the same place, so each half sorts
into the matching half of the room. On a tie `merge` takes from the left half,
and that is what makes the sort stable: `compiled` and `language` are both
eight letters long and come out in the order they went in.

## Two halves, two tasks

```sword
scope {
    for half in xs.chunks(mid) {
        spawn sortHalf(half)
    }
}
merge(xs, mid, scratch, before)
```

`xs.chunks(mid)` cuts the slice into pieces of `mid`, which for `mid` rounded
up from half the length is exactly two. Each task gets one piece to sort in
place, and the pieces are a partition: the race checker knows they cannot
overlap, so both tasks may write.

Slicing the two halves by hand says the same thing to a reader and is refused:

```sword
spawn sortHalf(xs[0..mid])
spawn sortHalf(xs[mid..n])
```

```
error: this writes 'xs' while it is already written concurrently
```

The checker does not do arithmetic on slice bounds. Proving `0..mid` and
`mid..n` apart is easy here and impossible in general, and a checker that
accepted the easy cases would refuse the next program for reasons nobody could
predict. `chunks` is the one construction that cannot produce overlapping
pieces, so it is the one accepted.

## An allocator per task

```sword
func sortHalf(mut half []i64) !void {
    mut sys := mem.NewSystem()
    mut scratch := mem.Alloc[i64](&sys, half.len) orelse return error.OutOfMemory
```

Each task needs room to merge, and the arena in `main` cannot be lent to both:
an allocator changes with every request, so handing the same one to two tasks
is a compile error. Each task makes its own, here a `mem.System` over C's
`malloc`, and frees what it took before it returns.

`sortHalf` can fail, and a failing task carries its error out to the scope:
if either half ran out of memory, `main` would fail at the closing brace. Once
the brace is passed, both halves are sorted, and the parent merges them with
the same `merge` the recursion uses.
