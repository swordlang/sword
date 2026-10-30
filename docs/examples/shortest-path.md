# Shortest paths

The shortest road from Lisbon to five other cities, found with Dijkstra's
algorithm over a binary heap written by hand.

Uses: [structs](../tour.md#structs), [methods](../tour.md#methods-and-interfaces),
[optionals](../tour.md#pointers-and-optionals),
[arrays and slices](../tour.md#arrays-and-slices), [memory](../tour.md#memory).

```sword
import "std/io"
import "std/mem"

struct Edge {
    from u64
    to   u64
    cost u64
}

struct Item {
    dist u64
    node u64
}

// A binary heap in a slice: the children of i are 2i+1 and 2i+2, and every
// parent is no farther than its children, so the nearest is always at 0.
struct Heap {
    items []Item
    len   u64
}

func (mut h *Heap) Push(it Item) {
    mut i := h.len
    h.items[i] = it
    h.len += 1
    for i > 0 {
        parent := (i - 1) / 2
        if h.items[parent].dist <= h.items[i].dist {
            break
        }
        h.swap(i, parent)
        i = parent
    }
}

func (mut h *Heap) Pop() ?Item {
    if h.len == 0 {
        return nil
    }
    top := h.items[0]
    h.len -= 1
    h.items[0] = h.items[h.len]
    mut i u64 = 0
    for {
        mut least := i
        for c in [2]u64{2 * i + 1, 2 * i + 2} {
            if c < h.len && h.items[c].dist < h.items[least].dist {
                least = c
            }
        }
        if least == i {
            break
        }
        h.swap(i, least)
        i = least
    }
    return top
}

func (mut h *Heap) swap(a u64, b u64) {
    t := h.items[a]
    h.items[a] = h.items[b]
    h.items[b] = t
}

const Far = 0xffffffffffffffff

func main() !void {
    names := [6]string{"Lisbon", "Madrid", "Paris", "Lyon", "Milan", "Rome"}
    // Road distances in km, both ways.
    roads := [8]Edge{
        Edge{from: 0, to: 1, cost: 625},
        Edge{from: 1, to: 2, cost: 1270},
        Edge{from: 1, to: 3, cost: 1050},
        Edge{from: 2, to: 3, cost: 465},
        Edge{from: 3, to: 4, cost: 440},
        Edge{from: 2, to: 4, cost: 850},
        Edge{from: 4, to: 5, cost: 575},
        Edge{from: 3, to: 5, cost: 1000},
    }

    mut backing := [4096]u8{}
    mut arena := mem.NewArena(backing[..])
    // A node can be pushed once per road into it, two ways each.
    mut heap := Heap{items: mem.Alloc[Item](&arena, 2 * roads.len + 1) orelse return error.OutOfMemory, len: 0}

    mut dist := [6]u64{Far, Far, Far, Far, Far, Far}
    mut via := [6]u64{0, 0, 0, 0, 0, 0}
    dist[0] = 0
    heap.Push(Item{dist: 0, node: 0})

    for it := heap.Pop() {
        // A node can be in the heap more than once; only its nearest entry counts.
        if it.dist > dist[it.node] {
            continue
        }
        for r in roads {
            mut next := r.to
            if r.from != it.node {
                if r.to != it.node {
                    continue
                }
                next = r.from
            }
            d := it.dist + r.cost
            if d < dist[next] {
                dist[next] = d
                via[next] = it.node
                heap.Push(Item{dist: d, node: next})
            }
        }
    }

    for n in 1..u64(6) {
        try io.Printf("{} km to {}:", dist[n], names[n])
        mut path := [6]u64{}
        mut steps u64 = 0
        mut at := n
        for at != 0 {
            path[steps] = at
            steps += 1
            at = via[at]
        }
        try io.Print(" Lisbon")
        for k in 0..steps {
            try io.Printf(" > {}", names[path[steps - 1 - k]])
        }
        try io.Print("\n")
    }
}
```
<!-- prints:
625 km to Madrid: Lisbon > Madrid
1895 km to Paris: Lisbon > Madrid > Paris
1675 km to Lyon: Lisbon > Madrid > Lyon
2115 km to Milan: Lisbon > Madrid > Lyon > Milan
2675 km to Rome: Lisbon > Madrid > Lyon > Rome
-->

## A heap in a slice

A binary heap is a tree kept in an array with no pointers: the children of
slot `i` are slots `2i + 1` and `2i + 2`. The one rule is that no parent is
farther than its children, so the nearest item is always in slot 0.

`Push` puts the new item at the end and swaps it with its parent while it is
nearer. `Pop` takes slot 0, moves the last item there, and swaps it down with
the nearer child until neither child is nearer. Both walk one path from a leaf
to the root, so both cost the logarithm of the size.

`Pop` answers `?Item`, nil when the heap is empty, and that is what lets the
main loop read `for it := heap.Pop()`: it runs while there is an item and stops
at the first nil, the same loop that drains a channel.

The heap's slice comes from an arena on `main`'s stack. Its size is worked out
in advance, one item per road into each city and the start, so `Push` never
needs to grow and the program allocates nothing from the system.

## The algorithm

Every city starts infinitely far, written `Far`, the largest `u64`. Lisbon is
at 0 and goes in the heap. Each time round, the nearest city not yet settled
comes off the heap, and every road out of it is tried: if going through it
reaches the other end sooner than anything found so far, that end gets the
shorter distance, remembers where it came from in `via`, and goes in the heap.

A city can be pushed again with a shorter distance while an older, longer
entry is still in the heap. Rather than finding and updating the old one, the
loop pushes the new one and skips the old when it comes out:
`if it.dist > dist[it.node]`.

## Reading the path back

`via` holds, for each city, the one before it on the shortest road. Following
it from the destination to Lisbon gives the road backwards, so it is collected
into `path` and printed from the end. Rome comes through Lyon, at 2675 km,
rather than through Milan at 2690.
