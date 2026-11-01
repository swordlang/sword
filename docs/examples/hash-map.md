# A hash map from scratch

A hash table written for any key and value: open addressing, doubling when it
fills, and every byte of it taken from an allocator the caller hands over.
`std/collections` has one; this is how such a thing is built.

Uses: [generics](../tour.md#generics), [structs](../tour.md#structs),
[methods](../tour.md#methods-and-interfaces), [memory](../tour.md#memory),
[optionals](../tour.md#pointers-and-optionals), [`hashof`](../reference.md#generics).

```sword
import "std/io"
import "std/mem"

struct Slot[K, V] {
    used  bool
    key   K
    value V
}

// Open addressing: every entry lives in the one array of slots, and a key
// that finds its slot taken tries the next one along.
struct Table[K, V] {
    slots []Slot[K, V]
    count u64
}

func newTable[K, V](mut a mem.Allocator, capacity u64) !Table[K, V] {
    mut slots := mem.Alloc[Slot[K, V]](a, capacity) orelse return error.OutOfMemory
    for i in 0..slots.len {
        slots[i].used = false
    }
    return Table[K, V]{slots: slots, count: 0}
}

// The slot that holds key, or the empty one where it would go. The capacity
// is a power of two, so the mask is the remainder without a division.
func (t *Table[K, V]) find(key K) u64 {
    mask := t.slots.len - 1
    mut at := hashof(key) & mask
    for t.slots[at].used && t.slots[at].key != key {
        at = (at + 1) & mask
    }
    return at
}

func (t *Table[K, V]) Get(key K) ?V {
    at := t.find(key)
    if !t.slots[at].used {
        return nil
    }
    return t.slots[at].value
}

func (mut t *Table[K, V]) Set(mut a mem.Allocator, key K, value V) !void {
    // Past three quarters full, probes get long: double first.
    if (t.count + 1) * 4 > t.slots.len * 3 {
        try t.grow(a)
    }
    at := t.find(key)
    if !t.slots[at].used {
        t.count += 1
    }
    t.slots[at] = Slot[K, V]{used: true, key: key, value: value}
}

func (mut t *Table[K, V]) grow(mut a mem.Allocator) !void {
    old := t.slots
    mut bigger := try newTable[K, V](a, old.len * 2)
    for s in old {
        if s.used {
            at := bigger.find(s.key)
            bigger.slots[at] = s
        }
    }
    bigger.count = t.count
    mem.Free(a, old)
    t.slots = bigger.slots
}

func (mut t *Table[K, V]) Free(mut a mem.Allocator) {
    mem.Free(a, t.slots)
}

func main() !void {
    mut sys := mem.NewSystem()

    mut squares := try newTable[u64, u64](&sys, 4)
    for i in 0..u64(1000) {
        try squares.Set(&sys, i, i * i)
    }
    try io.Printf("{} entries in {} slots; 999 squared is {}\n",
        squares.count, squares.slots.len, squares.Get(999) orelse 0)
    if v := squares.Get(1000) {
        try io.Printf("1000 is there: {}\n", v)
    } else {
        try io.Print("1000 is not there\n")
    }

    mut capitals := try newTable[string, string](&sys, 4)
    try capitals.Set(&sys, "Brazil", "Brasília")
    try capitals.Set(&sys, "Japan", "Tokyo")
    try capitals.Set(&sys, "Brazil", "Brasilia")
    try io.Printf("{} entries; Brazil: {}\n", capitals.count, capitals.Get("Brazil") orelse "?")

    squares.Free(&sys)
    capitals.Free(&sys)
    try io.Printf("{} bytes still held\n", sys.Live())
}
```
<!-- prints:
1000 entries in 2048 slots; 999 squared is 998001
1000 is not there
2 entries; Brazil: Brasilia
0 bytes still held
-->

## Slots

```sword
struct Slot[K, V] {
    used  bool
    key   K
    value V
}
```

The whole table is one slice of slots. An entry goes in the slot its hash
points at, and when that one is taken by another key, in the next free one
along: that is open addressing with linear probing. There are no lists hanging
off the slots, so a lookup reads memory in a straight line, which is what
caches are fast at.

`hashof(key)` is built in. It takes anything `==` can compare, an integer, a
string, an enum, and gives a `u64`; the generic code calls it without knowing
which. The capacity is kept a power of two, so `hashof(key) & mask` is the
remainder of a division without dividing.

`find` walks from the hashed slot until it meets the key or an empty slot.
Either way the answer is useful: `Get` reads the slot when it is used, and
`Set` writes into it whether it was used or not.

## Growing

```sword
if (t.count + 1) * 4 > t.slots.len * 3 {
    try t.grow(a)
}
```

As the table fills, runs of used slots grow and a probe walks further. Past
three quarters full, `Set` doubles the table first: a new slice from the
allocator, every entry placed again, since its slot depends on the size, and
the old slice given back with `mem.Free`. A thousand squares, starting from
four slots, end in 2048.

Growing can fail, because the allocator can run out. That is why `Set` is
`!void` and `newTable` is `!Table`: the program sees running out of memory as
an error at the line that caused it, not as a crash somewhere later.

## Who holds the allocator

`Set` and `Free` take the allocator as an argument rather than the table
keeping it. Either works; this way the type of the table says nothing about
memory, and a reader of any call that might allocate sees the allocator
passed to it. `std/collections` makes the other choice, and keeps it in the
map.

`main` uses a `mem.System`, which goes to C's `malloc` and counts what is
live. After both tables are freed it reports 0 bytes, so nothing leaked.

A key that is set twice keeps one slot: `Brazil` is there once, with the value
written last.
