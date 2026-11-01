# Reading a file

A file read two ways: all at once into an arena, and a piece at a time through
a buffer of sixteen bytes. The program writes the file it reads, so it runs
anywhere, and removes it when it is done.

Uses: [errors](../tour.md#errors), [`defer`](../tour.md#defer),
[memory](../tour.md#memory), [the outside world](../tour.md#the-outside-world),
[`std/fs`](../reference.md#stdfs).

```sword
import "std/fs"
import "std/io"
import "std/mem"

error LineTooLong = "a line longer than the buffer kept for one"

const Poem = "Folded steel\nholds an edge\nlong after\nthe fire is out\n"

// The whole file at once: one allocation, the size of the file.
func wholeFile(path string) !void {
    mut backing := [4096]u8{}
    mut arena := mem.NewArena(backing[..])
    text := string(try fs.ReadAll(path, &arena))
    mut lines u64 = 0
    for c in text {
        if c == "\n"[0] {
            lines += 1
        }
    }
    try io.Printf("read whole: {} bytes, {} lines\n", text.len, lines)
}

// A piece at a time through a small buffer, as a file of any size would be
// read. A line can be cut between two reads, so what is left of it waits in
// `line` until its end arrives.
func inPieces(path string) !void {
    mut f := try fs.Open(path, fs.Mode.Read)
    defer f.Close()

    mut buf := [16]u8{}
    mut line := [64]u8{}
    mut have u64 = 0
    mut reads u64 = 0
    mut number u64 = 0
    for {
        n := try f.Read(buf[..])
        if n == 0 {
            break
        }
        reads += 1
        for c in buf[0..n] {
            if c != "\n"[0] {
                if have == line.len {
                    return error.LineTooLong
                }
                line[have] = c
                have += 1
                continue
            }
            number += 1
            try io.Printf("  {}: {}\n", number, string(line[0..have]))
            have = 0
        }
    }
    try io.Printf("read in {} pieces of at most {} bytes\n", reads, buf.len)
}

func main() !void {
    mut room := [256]u8{}
    path := try fs.TempPath("/tmp/poem.", room[..])
    try fs.WriteAll(path, []u8(Poem))
    defer fs.Remove(path) catch {}

    try wholeFile(path)
    try inPieces(path)

    mut sys := mem.NewSystem()
    fs.ReadAll("no-such-file.txt", &sys) catch |e| {
        try io.Printf("no-such-file.txt: {} ({})\n", e.Message(), nameof(e))
        return
    }
}
```
<!-- prints:
read whole: 54 bytes, 4 lines
  1: Folded steel
  2: holds an edge
  3: long after
  4: the fire is out
read in 4 pieces of at most 16 bytes
no-such-file.txt: the file could not be opened (CannotOpen)
-->

## All at once

```sword
text := string(try fs.ReadAll(path, &arena))
```

`fs.ReadAll` asks the file's size, takes that much from the allocator, and
reads it in. It is the simplest way to read a file and the right one for a
file that fits in memory comfortably: a configuration, a source file, a page.

The bytes live in the arena, and the arena lives in `wholeFile`'s frame, so
the text is gone when the function returns. Here the arena is 4 KiB on the
stack; a file bigger than that fails with `error.OutOfMemory` instead of being
cut short, and a program expecting large files hands `ReadAll` an allocator
with room, such as a `mem.System`.

`string(...)` turns the `[]u8` into a string without copying: both are a
pointer and a length. A `for c in text` then walks it byte by byte.

## A piece at a time

```sword
mut f := try fs.Open(path, fs.Mode.Read)
defer f.Close()
```

`fs.Open` gives a `File`, and `defer` closes it however the function ends: at
the bottom, at a `return`, or at a `try` that fails halfway through. `Read`
fills as much of the buffer as it can and answers how many bytes it put
there, and 0 means the end of the file.

Sixteen bytes is far too small for real use and right for showing what
happens: the 54 bytes arrive in four reads, and the second line starts in one
read and ends in the next. Nothing in a read lines up with a line, so the
bytes of the line being read are kept in `line` until its newline comes, and
only then printed. A line longer than `line` has room for is an error of its
own rather than a write past the end.

The memory this takes is the two buffers, 80 bytes, whatever the size of the
file. That is the reason to read a file this way: a log of ten gigabytes goes
through the same 80 bytes.

## When the file is not there

```sword
fs.ReadAll("no-such-file.txt", &sys) catch |e| {
```

A file that cannot be opened is `error.CannotOpen`, with a message to print.
`catch |e|` names the error so the program can say which it was: `e.Message()`
is the sentence and `nameof(e)` the name.

## Writing it in the first place

`fs.TempPath` makes a name that is not taken, from the prefix it is given,
into a buffer of the caller's, and `fs.WriteAll` creates the file with the
poem in it. The `defer fs.Remove(path) catch {}` right after removes it when
`main` returns. `Remove` can fail, and a failure in Sword cannot be dropped
without a word: written without the `catch {}`, the line is refused with
"!void cannot be discarded; handle it with 'try' or 'catch'". The empty
`catch` is the program saying, in writing, that a file it cannot remove is
nothing to stop for.
