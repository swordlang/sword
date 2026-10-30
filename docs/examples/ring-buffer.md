# A ring buffer

A queue of fixed capacity that keeps the most recent values and pushes out the
oldest. Its storage is an array inside the struct, so it never allocates.

Uses: [generics](../tour.md#generics), [structs](../tour.md#structs),
[methods](../tour.md#methods-and-interfaces),
[optionals](../tour.md#pointers-and-optionals).

```sword
import "std/io"

// The last four values pushed, oldest first. The array is part of the struct,
// so a Ring lives wherever its owner puts it and never allocates.
struct Ring[T] {
    items [4]T
    start u64      // where the oldest is
    count u64
}

// Adds v, pushing out the oldest when full; answers what was pushed out.
func (mut r *Ring[T]) Push(v T) ?T {
    end := (r.start + r.count) % r.items.len
    if r.count < r.items.len {
        r.items[end] = v
        r.count += 1
        return nil
    }
    old := r.items[r.start]
    r.items[r.start] = v
    r.start = (r.start + 1) % r.items.len
    return old
}

func (mut r *Ring[T]) Pop() ?T {
    if r.count == 0 {
        return nil
    }
    v := r.items[r.start]
    r.start = (r.start + 1) % r.items.len
    r.count -= 1
    return v
}

// The i-th oldest.
func (r *Ring[T]) At(i u64) T {
    return r.items[(r.start + i) % r.items.len]
}

func main() !void {
    mut recent := Ring[string]{items: [4]string{"", "", "", ""}, start: 0, count: 0}
    for line in [6]string{"GET /", "GET /about", "POST /login", "GET /", "GET /admin", "POST /logout"} {
        if gone := recent.Push(line) {
            try io.Printf("forgot {}\n", gone)
        }
    }
    try io.Print("the last four:\n")
    for i in 0..recent.count {
        try io.Printf("  {}\n", recent.At(i))
    }

    // The same type over numbers: a moving average of the last four readings.
    mut window := Ring[i64]{items: [4]i64{0, 0, 0, 0}, start: 0, count: 0}
    mut sum i64 = 0
    try io.Print("averages:")
    for reading in [7]i64{10, 20, 30, 40, 50, 60, 70} {
        sum += reading
        if gone := window.Push(reading) {
            sum -= gone
        }
        try io.Printf(" {}", sum / i64(window.count))
    }
    try io.Print("\ntaken out:")
    for v := window.Pop() {
        try io.Printf(" {}", v)
    }
    try io.Printf(", and {} left\n", window.count)
}
```
<!-- prints:
forgot GET /
forgot GET /about
the last four:
  POST /login
  GET /
  GET /admin
  POST /logout
averages: 10 15 20 25 35 45 55
taken out: 40 50 60 70, and 0 left
-->

## Storage inside the value

```sword
struct Ring[T] {
    items [4]T
    start u64
    count u64
}
```

`[4]T` is an array, a value, not a slice pointing elsewhere: a `Ring[string]`
is four strings and two numbers in one block. Declared in `main`, it lives in
`main`'s frame; put in another struct, it lives inside that. There is nothing
to allocate and nothing to free, which is why this is the shape for a buffer
that has to work where an allocator is not welcome, such as a logger that must
keep going when memory has run out.

## Going round

`start` is where the oldest value is, and the values run from there for
`count` slots, wrapping past the end of the array back to 0: slot
`(start + i) % items.len` is the i-th oldest. Pushing into a full ring
overwrites the oldest and moves `start` one along, so the next oldest becomes
the oldest. Nothing is ever moved.

`Push` answers `?T`: what it pushed out, or nil when there was room. The
caller decides whether that matters. The log prints what it forgot; the moving
average subtracts it from the running sum, so each average costs one addition
and one subtraction however long the window.

`Pop` takes from the oldest end, so a ring is also a queue: the values come
out in the order they went in.

## One type, two uses

The same `Ring[T]` holds request lines and integer readings. Each use is
compiled for its own `T`, so the strings are stored as strings and the numbers
as numbers, with no boxing and no interface in between.
