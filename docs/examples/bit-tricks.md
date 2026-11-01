# Bit tricks

A set of numbers kept as bits, the primes below 256 found with it, a count of
set bits that skips the zeros, and permissions packed into an integer the way a
file mode is.

Uses: [values](../tour.md#values), [methods](../tour.md#methods-and-interfaces),
[control flow](../tour.md#control-flow), [operators](../reference.md#operators).

```sword
import "std/io"

// A set of small numbers, one bit each, 64 to a word.
struct Bitset {
    words [4]u64
}

func (mut s *Bitset) Add(n u64) {
    s.words[n / 64] |= 1 << (n % 64)
}

func (mut s *Bitset) Remove(n u64) {
    s.words[n / 64] &= ~(1 << (n % 64))
}

func (s *Bitset) Has(n u64) bool {
    return s.words[n / 64] >> (n % 64) & 1 == 1
}

func (s *Bitset) Count() u64 {
    mut total u64 = 0
    for w in s.words {
        total += ones(w)
    }
    return total
}

// Each pass clears the lowest bit that is set, so the loop runs once per one
// rather than once per bit.
func ones(w u64) u64 {
    mut x := w
    mut n u64 = 0
    for x != 0 {
        x &= x - 1
        n += 1
    }
    return n
}

// Permissions packed into one integer, the way a file mode is.
const Read = 4
const Write = 2
const Exec = 1

func describe(mode u32) !void {
    for shift in [3]u32{6, 3, 0} {
        bits := mode >> shift & 7
        mut text := [3]u8{0x2d, 0x2d, 0x2d}
        if bits & Read != 0 {
            text[0] = "r"[0]
        }
        if bits & Write != 0 {
            text[1] = "w"[0]
        }
        if bits & Exec != 0 {
            text[2] = "x"[0]
        }
        try io.Print(string(text[..]))
    }
    try io.Print("\n")
}

func main() !void {
    mut primes := Bitset{words: [4]u64{}}
    for n in 2..u64(256) {
        primes.Add(n)
    }
    for n in 2..u64(16) {
        if primes.Has(n) {
            mut m := n * n
            for m < 256 {
                primes.Remove(m)
                m += n
            }
        }
    }
    try io.Printf("{} primes below 256, the last word {x}\n", primes.Count(), primes.words[3])

    try describe(0o755)
    try describe(0o640)
}
```
<!-- prints:
54 primes below 256, the last word 8028228800800a2
rwxr-xr-x
rw-r-----
-->

## A bitset

```sword
s.words[n / 64] |= 1 << (n % 64)
```

Number `n` is bit `n % 64` of word `n / 64`. Adding it sets that bit with `|`,
removing it clears the bit with `& ~`, and asking for it shifts the word right
until the bit is at the bottom and masks everything else off with `& 1`. Four
words hold 256 numbers in 32 bytes, where a `[256]bool` takes 256.

The `1` takes its type from the word it goes into, so it is a `u64` and can be
shifted into bit 63. `~` is the complement, every bit flipped; there is no
binary `~`, and xor is `^`.

The sieve marks every number from 2 to 255 as a candidate, then, for each
prime up to 15, removes its multiples from its square up. 16 × 16 is 256, so
by then every composite below 256 has met a factor. 54 are left, and the top
word printed in hex is the primes from 192 to 255, one bit each.

## Counting bits

```sword
x &= x - 1
```

Subtracting 1 turns the lowest set bit into a 0 and every 0 below it into a 1,
so the `&` with the original clears exactly that bit. The loop runs once per set
bit, not once per bit of the word: a sparse word costs almost nothing.

## Flags in an integer

```sword
const Read = 4
const Write = 2
const Exec = 1
```

`0o755` is an octal literal, three bits to a digit, and a file mode is three
such digits: the owner, the group, everyone else. `mode >> shift & 7` takes one
digit out, and `bits & Read != 0` asks for one permission within it.

That last expression needs no parentheses. In Sword `&` binds as tightly as
`*`, above the comparisons, so it reads the way it is meant. In C the same line
is `bits & (Read != 0)`, a classic bug.

`text` starts as three `-` bytes, `0x2d`, and each permission that is there
replaces its own with a letter, `"r"[0]` being the byte of `r`. `string(text[..])`
reads the three bytes as a string to print them.
