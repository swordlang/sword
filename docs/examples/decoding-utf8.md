# Decoding UTF-8

UTF-8 taken apart by hand: how many bytes a character takes, the bits each
byte carries, and the six ways bytes can fail to be UTF-8. Every character the
decoder accepts is checked against `std/unicode`.

Uses: [strings](../tour.md#strings), [errors](../tour.md#errors),
[structs](../tour.md#structs), [`std/unicode`](../reference.md#stdunicode).

```sword
import "std/io"
import "std/unicode"

error Truncated = "a character cut off by the end of the text"
error BadByte = "a byte that cannot start a character"
error BadFollow = "a byte inside a character that is not 10xxxxxx"
error Overlong = "a character written in more bytes than it needs"
error Surrogate = "half of a UTF-16 pair, which UTF-8 never carries"
error TooBig = "a code point past U+10FFFF"

struct Char {
    code u32
    size u64
}

// The first byte says how many follow: 0xxxxxxx stands alone, 110xxxxx takes
// one more, 1110xxxx two, 11110xxx three. Every byte after it is 10xxxxxx and
// carries six bits.
func decode(s []u8, at u64) !Char {
    b := s[at]
    mut size u64 = 0
    mut code u32 = 0
    if b < 0x80 {
        return Char{code: u32(b), size: 1}
    } else if b & 0xe0 == 0xc0 {
        size = 2
        code = u32(b & 0x1f)
    } else if b & 0xf0 == 0xe0 {
        size = 3
        code = u32(b & 0x0f)
    } else if b & 0xf8 == 0xf0 {
        size = 4
        code = u32(b & 0x07)
    } else {
        return error.BadByte
    }
    if at + size > s.len {
        return error.Truncated
    }
    for i in 1..size {
        next := s[at + i]
        if next & 0xc0 != 0x80 {
            return error.BadFollow
        }
        code = code << 6 | u32(next & 0x3f)
    }
    // The smallest code point each length is allowed to carry; anything
    // below could have been written shorter.
    least := [5]u32{0, 0, 0x80, 0x800, 0x10000}
    if code < least[size] {
        return error.Overlong
    }
    if code >= 0xd800 && code <= 0xdfff {
        return error.Surrogate
    }
    if code > 0x10ffff {
        return error.TooBig
    }
    return Char{code: code, size: size}
}

func walk(text string) !void {
    s := []u8(text)
    mut at u64 = 0
    for at < s.len {
        c := decode(s, at) catch |e| {
            try io.Printf("  byte {}: {}\n", at, e.Message())
            return
        }
        // The library agrees on every character this decoder accepts.
        r := unicode.Decode(text, at) orelse return error.BadByte
        if r.Code != c.code || r.Bytes != c.size {
            return error.BadByte
        }
        try io.Printf("  {}  0x{x}, size {}\n", text[at..at + c.size], c.code, c.size)
        at += c.size
    }
}

func main() !void {
    try io.Print("aé€𝄞\n")
    try walk("aé€𝄞")
    try io.Print("overlong slash\n")
    try walk("\xc0\xaf")
    try io.Print("cut short\n")
    try walk("ok\xe2\x82")
}
```
<!-- prints:
aé€𝄞
  a  0x61, size 1
  é  0xe9, size 2
  €  0x20ac, size 3
  𝄞  0x1d11e, size 4
overlong slash
  byte 0: a character written in more bytes than it needs
cut short
  o  0x6f, size 1
  k  0x6b, size 1
  byte 2: a character cut off by the end of the text
-->

## The first byte

| first byte | bytes | bits of the code point |
|---|---|---|
| `0xxxxxxx` | 1 | 7 |
| `110xxxxx` | 2 | 11 |
| `1110xxxx` | 3 | 16 |
| `11110xxx` | 4 | 21 |

The top bits of the first byte say how long the character is, and the rest of
it is the start of the code point. `b & 0xe0 == 0xc0` keeps the top three bits
and asks whether they are `110`; `b & 0x1f` keeps the five below them. `&`
binds tighter than `==` in Sword, so the test reads as it is written.

Each byte after the first is `10xxxxxx` and carries six more bits, shifted in
from the right: `code << 6 | u32(next & 0x3f)`.

## What is refused

A `10xxxxxx` byte cannot start a character, and neither can `11111xxx`: both
are `BadByte`. A byte inside a character that is not `10xxxxxx` is
`BadFollow`. A character whose bytes run past the end of the text is
`Truncated`, which is what the last test hits: `\xe2\x82` is the first two of
the three bytes of `€`.

The other three are bytes that decode to a number but must not be accepted.
An overlong form writes a code point in more bytes than it needs: `\xc0\xaf`
decodes to `0x2f`, the slash, which is one byte. Accepting it once let a path
like `..\xc0\xaf..` through a filter looking for `../`, so the standard says
every character has exactly one encoding, and `least` holds the smallest code
point each length may carry. The code points `0xd800` to `0xdfff` are halves of
UTF-16 pairs and never stand alone, and nothing is above `0x10ffff`.

## Checking against the library

```sword
r := unicode.Decode(text, at) orelse return error.BadByte
```

`unicode.Decode` answers the code point and its length, or nil for anything
that is not a character starting at `at`. The program asks it about every
character the hand decoder accepted and stops if the two ever disagree. A
second implementation to compare with is the cheapest test a decoder can have.

The decoder works on `[]u8` and slices it to print each character,
`text[at..at + c.size]`. Slicing a string is by byte, never by character, so a
slice that ended inside `€` would print half of it, which is the reason to know
where each character ends.
