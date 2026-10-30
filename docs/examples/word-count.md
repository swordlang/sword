# Counting words

Reads text from standard input, counts every word in a map, and prints the
five most frequent. With nothing on standard input it counts a sample.

Uses: [strings](../tour.md#strings), [memory](../tour.md#memory),
[function values](../tour.md#function-values),
[`std/collections`](../reference.md#stdcollections), [`std/fs`](../reference.md#stdfs).

```sword
import "std/collections"
import "std/fs"
import "std/io"
import "std/mem"
import "std/strings"

const Sample = "The race checker is the point. Within a scope, two accesses to the same memory are an error unless both only read, or they are pieces of one partition. The checker reads the signature, and the signature says who writes."

struct Count {
    word string
    n    u64
}

func min(a u64, b u64) u64 {
    if a < b {
        return a
    }
    return b
}

func letter(c u8) bool {
    return c >= "a"[0] && c <= "z"[0]
}

// More often first; equal counts in alphabetical order, so the output does
// not depend on where the map happened to keep each word.
func ranks(a Count, b Count) bool {
    if a.n != b.n {
        return a.n > b.n
    }
    return before(a.word, b.word)
}

// Byte by byte, which for lower-case ASCII is alphabetical.
func before(a string, b string) bool {
    for i in 0..a.len {
        if i == b.len {
            return false
        }
        if a[i] != b[i] {
            return a[i] < b[i]
        }
    }
    return a.len < b.len
}

func main() !void {
    mut sys := mem.NewSystem()

    mut text := try fs.ReadStdin(&sys, 1 << 24)
    if text.len == 0 {
        text = mem.Alloc[u8](&sys, Sample.len) orelse return error.OutOfMemory
        for i in 0..Sample.len {
            text[i] = Sample[i]
        }
    }
    for i in 0..text.len {
        text[i] = strings.ToLower(text[i])
    }

    // Words are runs of letters; each is a view into text, not a copy.
    mut counts := try collections.NewMap[u64](&sys, 64)
    mut total u64 = 0
    mut at u64 = 0
    for at < text.len {
        for at < text.len && !letter(text[at]) {
            at += 1
        }
        start := at
        for at < text.len && letter(text[at]) {
            at += 1
        }
        if at > start {
            word := string(text[start..at])
            try counts.Set(word, (counts.Get(word) orelse 0) + 1)
            total += 1
        }
    }

    mut list := try collections.NewList[Count](&sys, counts.Len())
    for i in 0..counts.Slots() {
        if word := counts.KeyAt(i) {
            try list.Push(Count{word: word, n: counts.ValueAt(i)})
        }
    }
    mut sorted := list.Slice()
    collections.Sort(sorted, ranks)

    try io.Printf("{} words, {} different\n", total, counts.Len())
    for i in 0..min(5, sorted.len) {
        try io.Printf("{} {}\n", sorted[i].n, sorted[i].word)
    }
}
```
<!-- prints:
40 words, 32 different
6 the
2 are
2 checker
2 signature
1 a
-->

```
$ echo "the cat and the hat. The end" | ./words
7 words, 5 different
3 the
1 and
1 cat
1 end
1 hat
```

## Reading

`fs.ReadStdin` reads all of standard input into memory from the allocator it
is given, up to a limit, here 16 MiB. A program reading input it does not
control says how much it is willing to hold; past the limit it fails rather
than taking whatever is sent.

The text is lowered in place, one byte at a time. It is ASCII-only on purpose:
a word with an accent is cut at the accent, and the [UTF-8
example](decoding-utf8.md) is where characters beyond ASCII are taken apart.

## Words are views

```sword
word := string(text[start..at])
```

A word is a slice of `text`, the bytes already read, not a copy of them. The
map stores the string, which is a pointer and a length, so a thousand
occurrences of `the` cost one entry pointing into the text. That holds as long
as `text` lives, and here it lives until `main` returns.

`(counts.Get(word) orelse 0) + 1` reads the count so far, or 0 the first time.
The parentheses are needed: `orelse` binds below everything, so without them
the `+ 1` would go with the 0.

## Sorting

A map has no order, so the counts are copied into a list and sorted with
`collections.Sort`, which takes the comparison as a function value. `ranks`
puts the larger count first, and breaks ties by the word itself. Without that,
the words seen twice would come out in whatever order the map kept them, which
depends on their hashes, and a change to the hash would change the output.

`collections.Sort` is not stable, which is one more reason to make the
comparison decide every tie.
