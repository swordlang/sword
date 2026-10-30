# Reading a binary format

The header of a WAV file read field by field from bytes: fixed offsets,
little-endian integers put together with shifts, and an error for each way a
header can be wrong.

Uses: [errors](../tour.md#errors), [arrays and slices](../tour.md#arrays-and-slices),
[string literals](../reference.md#string-literals),
[operators](../reference.md#operators), [conversions](../reference.md#conversions).

```sword
import "std/io"

error TooShort = "fewer bytes than a WAV header"
error NotRiff = "does not start with RIFF"
error NotWave = "a RIFF file, but not WAVE"
error NoFormat = "the fmt chunk is missing or the wrong size"
error NotPcm = "compressed audio, not PCM"
error Inconsistent = "the byte rate does not match the rest"

struct Wav {
    channels u16
    rate     u32
    bits     u16
    frames   u32
}

// WAV is little-endian: the low byte comes first.
func u16At(b []u8, at u64) u16 {
    return u16(b[at]) | u16(b[at + 1]) << 8
}

func u32At(b []u8, at u64) u32 {
    return u32(b[at]) | u32(b[at + 1]) << 8 | u32(b[at + 2]) << 16 | u32(b[at + 3]) << 24
}

func tagAt(b []u8, at u64, tag string) bool {
    return string(b[at..at + 4]) == tag
}

func parse(b []u8) !Wav {
    if b.len < 44 {
        return error.TooShort
    }
    if !tagAt(b, 0, "RIFF") {
        return error.NotRiff
    }
    if !tagAt(b, 8, "WAVE") {
        return error.NotWave
    }
    if !tagAt(b, 12, "fmt ") || u32At(b, 16) != 16 {
        return error.NoFormat
    }
    if u16At(b, 20) != 1 {
        return error.NotPcm
    }
    channels := u16At(b, 22)
    rate := u32At(b, 24)
    bits := u16At(b, 34)
    frame := u32(channels) * u32(bits / 8)
    if u32At(b, 28) != rate * frame || u32(u16At(b, 32)) != frame {
        return error.Inconsistent
    }
    return Wav{channels: channels, rate: rate, bits: bits, frames: u32At(b, 40) / frame}
}

func show(name string, b []u8) !void {
    w := parse(b) catch |e| {
        try io.Printf("{}: {}\n", name, e.Message())
        return
    }
    try io.Printf("{}: {} channels, {} Hz, {} bits, {} frames\n",
        name, w.channels, w.rate, w.bits, w.frames)
}

func main() !void {
    // The first 44 bytes of one second of CD audio.
    cd := "RIFF\x34\xb1\x02\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x02\x00\x44\xac\x00\x00\x10\xb1\x02\x00\x04\x00\x10\x00data\x10\xb1\x02\x00"
    try show("cd", []u8(cd))

    // The same, claiming 48 kHz without changing the byte rate.
    mut wrong := [44]u8{}
    for i in 0..cd.len {
        wrong[i] = cd[i]
    }
    wrong[24] = 0x80
    wrong[25] = 0xbb
    try show("edited", wrong[..])

    try show("cut", []u8(cd[0..20]))
}
```
<!-- prints:
cd: 2 channels, 44100 Hz, 16 bits, 44100 frames
edited: the byte rate does not match the rest
cut: fewer bytes than a WAV header
-->

## The layout

A WAV file starts with 44 bytes at fixed places: the tag `RIFF`, the size of
the rest, the tag `WAVE`, a `fmt ` chunk of 16 bytes describing the sound, and
the tag `data` with the number of bytes of sound after it. Reading it is
looking at known offsets, which is why every access in `parse` is a number.

The sample header is a string literal written with `\xNN` escapes, one per byte
that is not printable. A string is bytes, and `[]u8(cd)` is the same bytes seen
as a slice.

## Little-endian

```sword
func u32At(b []u8, at u64) u32 {
    return u32(b[at]) | u32(b[at + 1]) << 8 | u32(b[at + 2]) << 16 | u32(b[at + 3]) << 24
}
```

WAV stores the low byte first, so 44100, which is `0xac44`, is the bytes `44 ac`.
Each byte is widened to `u32` before it is shifted. A shift keeps the type of
what is shifted, so a `u8` shifted left by 4 quietly loses its top bits, and one
shifted by 8, its whole width, panics. Widening first gives the bits somewhere
to go.

`<<` binds tighter than `|`, so the line needs no parentheses. The order is in
[the operator table](../reference.md#operators).

Assembling the bytes one at a time, rather than reading four of them through a
`u32` pointer, gives the same answer on a processor of either byte order and at
an address of any alignment.

## An error per field

```sword
error NotPcm = "compressed audio, not PCM"
```

Each way a header can be wrong is an error of its own, declared with the
message a person would want to read. `parse` returns the first one it finds, and
`show` prints its message with `e.Message()`. A caller that wants to act on
one rather than print it compares it, `if e == error.NotPcm`, or puts it
through a `switch` with a `case error.NotPcm:`.

The length is checked once, at the top: once `b.len < 44` is ruled out, every
offset below is inside the slice.

The edited header changes the sample rate to 48000 and leaves the byte rate at
44100 × 4. The two disagree, and a reader that trusted either would play the
file at the wrong speed, which is what `Inconsistent` is for.
