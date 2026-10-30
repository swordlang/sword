# Your own allocator

Two allocators written from scratch: one that bumps through a buffer, and one
that wraps any other and counts what passes through it.

Uses: [interfaces](../tour.md#methods-and-interfaces),
[memory](../tour.md#memory), [optionals](../tour.md#pointers-and-optionals),
[`std/mem`](../reference.md#stdmem).

```sword
import "std/io"
import "std/mem"

// Bump hands out memory from a buffer it was given, front to back, and takes
// none of it back until Reset.
struct Bump {
    buf  []u8
    next u64
}

func (mut b *Bump) Alloc(n u64, align u64) ?[*]u8 {
    // The address has to be aligned, not the offset: the buffer itself may
    // start anywhere.
    base := u64(b.buf.ptr)
    at := mem.Align(base + b.next, align) - base
    if at > b.buf.len || n > b.buf.len - at {
        return nil
    }
    b.next = at + n
    return b.buf[at..at + n].ptr
}

func (mut b *Bump) Release(mut p [*]u8, n u64) {
}

func (mut b *Bump) Reset() {
    b.next = 0
}

// Counting passes every request on to another allocator and keeps score.
struct Counting {
    inner mem.Allocator
    live  u64
    peak  u64
    calls u64
}

func (mut c *Counting) Alloc(n u64, align u64) ?[*]u8 {
    p := c.inner.Alloc(n, align) orelse return nil
    c.calls += 1
    c.live += n
    if c.live > c.peak {
        c.peak = c.live
    }
    return p
}

func (mut c *Counting) Release(mut p [*]u8, n u64) {
    c.inner.Release(p, n)
    c.live -= n
}

func main() !void {
    mut backing := [256]u8{}
    mut bump := Bump{buf: backing[..], next: 0}
    mut counted := Counting{inner: &bump, live: 0, peak: 0, calls: 0}

    a := mem.Alloc[u8](&counted, 3) orelse return error.OutOfMemory
    b := mem.Alloc[u64](&counted, 4) orelse return error.OutOfMemory
    try io.Printf("{} u8s, then {} u64s on a multiple of 8: {}\n", a.len, b.len, u64(b.ptr) % 8 == 0)

    mem.Free(&counted, b)
    try io.Printf("{} calls, {} live, {} at most\n", counted.calls, counted.live, counted.peak)

    if big := mem.Alloc[u64](&counted, 100) {
        try io.Printf("got {}\n", big.len)
    } else {
        try io.Print("100 u64s do not fit in 256 bytes\n")
    }
}
```
<!-- prints:
3 u8s, then 4 u64s on a multiple of 8: true
2 calls, 3 live, 35 at most
100 u64s do not fit in 256 bytes
-->

## The interface

Everything in Sword that touches the heap takes a `mem.Allocator`, and the
interface is two methods:

```sword
interface Allocator {
    mut Alloc(n u64, align u64) ?[*]u8
    mut Release(mut p [*]u8, n u64)
}
```

A struct with those two methods is an allocator. Nothing has to say so: `Bump`
and `Counting` never mention `Allocator`, and `&bump` goes where one is wanted.
Both methods are `mut` in the interface because handing out memory changes the
allocator, and so the implementations take `mut b *Bump`.

`Alloc` answers nil when it cannot, rather than failing with an error. Running
out is ordinary for an allocator with a fixed buffer, and the caller decides
what it means: `orelse return error.OutOfMemory` in `main`, a smaller request,
or a message, as the last lines of the program do.

## Bumping

```sword
base := u64(b.buf.ptr)
at := mem.Align(base + b.next, align) - base
```

`mem.Align` rounds a number up to the next multiple of the alignment. The
first request is three `u8`s, which need no alignment. The second is four
`u64`s, which must sit at an address that is a multiple of 8, so the bytes up
to the next one are skipped.

It is the address that gets rounded, not the offset into the buffer. A `[256]u8`
holds bytes, and nothing makes it start on a multiple of 8; rounding the offset
alone would hand back a misaligned `u64` whenever it did not. Ordinary loads on
most processors forgive that, slowly. An atomic on arm64 does not, and a
`shared` value placed there faults on its own lock.

The bounds check is written as `n > b.buf.len - at` rather than
`at + n > b.buf.len`: a huge `n` would overflow the sum, and a request that
cannot be met should answer nil, not stop the program.

`Release` does nothing. A bump allocator gets its memory back all at once,
with `Reset`, and that is the reason to use one: one request per call at the
cost of an addition, and freeing a thousand objects costs the same as freeing
one. `mem.Arena` in the standard library is this allocator, with a few more
methods.

## Wrapping

`Counting` holds another allocator in a field, `inner mem.Allocator`, and passes
every call on to it. An interface value is a pointer to the data and a pointer
to its methods, so the field is two words, whatever allocator is behind it.

The counts come out as 2 calls, 3 bytes live and 35 at most: 3 bytes, then 32
more for the `u64`s, then 32 given back by `mem.Free`, which calls `Release`
with the size the slice was asked for. The padding between them
is the bump allocator's business, and a wrapper does not see it.

The same shape serves any allocator that watches another: one that fails the
hundredth request to test what a program does when memory runs out, one that
logs, or `mem.Watched`, which overwrites memory as it is released so that
anything still reading it reads garbage.
