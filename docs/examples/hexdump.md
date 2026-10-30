# A hexdump

Bytes shown the way `hexdump -C` shows them: the offset, sixteen bytes in hex,
and the same bytes as text.

Uses: [arrays and slices](../tour.md#arrays-and-slices),
[methods and interfaces](../tour.md#methods-and-interfaces),
[`std/fmt`](../reference.md#stdfmt), [`std/io`](../reference.md#stdio).

```sword
import "std/fmt"
import "std/io"

const Row = 16

// Sixteen bytes a line: the offset, the bytes in hex, then the same bytes as
// text with anything unprintable shown as a dot.
func dump(mut out fmt.Sink, data []u8) !void {
    mut at u64 = 0
    for at < data.len {
        mut end := at + Row
        if end > data.len {
            end = data.len
        }
        line := data[at..end]

        try fmt.Hex(out, at, 8)
        try fmt.Str(out, " ")
        for i in 0..u64(Row) {
            if i == 8 {
                try fmt.Str(out, " ")
            }
            if i < line.len {
                try fmt.Str(out, " ")
                try fmt.Hex(out, u64(line[i]), 2)
            } else {
                try fmt.Str(out, "   ")
            }
        }
        try fmt.Str(out, "  |")
        for c in line {
            if c >= 0x20 && c < 0x7f {
                try out.WriteByte(c)
            } else {
                try out.WriteByte(0x2e)
            }
        }
        try fmt.Str(out, "|\n")
        at = end
    }
}

func main() !void {
    mut out := io.NewWriter(1)
    try dump(&out, []u8("Sword\tcompiles\nto native code.\x00\xff\x7f"))
    try out.Flush()
}
```
<!-- prints:
00000000  53 77 6f 72 64 09 63 6f  6d 70 69 6c 65 73 0a 74  |Sword.compiles.t|
00000010  6f 20 6e 61 74 69 76 65  20 63 6f 64 65 2e 00 ff  |o native code...|
00000020  7f                                                |.|
-->

## Writing into a sink

`dump` does not print. It writes into a `fmt.Sink`, the interface every
formatting function in `std/fmt` writes into, and `main` hands it an
`io.Writer` over descriptor 1, standard output. The same `dump` would fill a
`bytes.Buffer`, to be compared in a test, or a socket, with no change.

`io.Writer` buffers what it is given and writes it out in large pieces, so a
dump of a large file is a few system calls rather than one per byte. What it
still holds at the end goes out with `Flush`, and a program that forgets the
`Flush` loses its last line.

## Hex of a fixed width

```sword
try fmt.Hex(out, at, 8)
```

`fmt.Hex` writes a number in hexadecimal, padded with zeros to the width asked
for: 8 digits for the offset, 2 for each byte. `{x}` in a `Printf` format is
the same digits with no padding, which suits a number on its own and not a
column.

## The last line

The text here is 33 bytes, so the third line holds one byte and fifteen gaps.
The loop over the columns runs to 16 whatever the line holds, writing three
spaces for each byte that is not there, so the text column lines up with the
lines above it. An extra space after the eighth column splits each line in two
halves of eight, as `hexdump -C` does.

`end` starts as `at + Row` and is pulled back to the length on the last line.
It is declared `mut` because it is assigned twice; everything else in the loop
is set once.

## Printable bytes

```sword
if c >= 0x20 && c < 0x7f {
```

There are no character literals in Sword: a byte is a number, and the printable
range of ASCII is written as one, from the space to the tilde. Anything outside
it, the tab and newline in the text included, is shown as a dot, `0x2e`. A byte
from a string can be had with `"."[0]` when that reads better.
