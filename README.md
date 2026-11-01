# Sword

Sword is a compiled systems language with Go's syntax and Zig's memory model.
It has no garbage collector, nothing allocates behind your back, and the
compiler refuses to build a program where two tasks can touch the same memory
at the same time.

```sword
import "std/mem"

func square(xs []i64, mut out []i64) {
    for i in 0..out.len {
        out[i] = xs[i] * xs[i]
    }
}

func main() !int {
    mut backing := [4096]u8{}
    mut arena := mem.NewArena(backing[..])

    mut src := mem.Alloc[i64](&arena, 64) orelse return error.OutOfMemory
    mut dst := mem.Alloc[i64](&arena, 64) orelse return error.OutOfMemory
    for i in 0..src.len {
        src[i] = i64(i)
    }

    scope {
        for off, part in dst.chunks(8) {
            spawn square(src[off..off+part.len], part)
        }
    }

    return int(dst[63] % 100)
}
```
<!-- exits: 69 -->

Eight tasks write into eight pieces of the same array. That compiles because
`chunks` hands out pieces that cannot overlap. Slice `dst` by hand inside the
loop instead and the compiler stops you, because then it has no way to know the
pieces are distinct.

What the compiler cannot see is the order tasks run in, so the test runner
varies it on purpose: `shield test -sim` runs each test under many
interleavings, one thread and a seed each, and a failure comes with the seed
that makes it happen again.

## Install

```sh
git clone https://github.com/swordlang/sword
cd sword
make
make install
```

That builds the latest release, 0.4.0 "Temper", from `main`; `develop` has
what is coming next. What each release changed is in
[CHANGELOG.md](CHANGELOG.md).

```sword
import "std/io"

func main() !int {
    try io.Printf("{} workers, {.1}ms each\n", 8, 12.5)
    return 0
}
```
<!-- prints:
8 workers, 12.5ms each
-->

That puts `shield` (the compiler) and `swordls` (the language server) in
`~/.local/bin`. See [docs/install.md](docs/install.md) for prefixes, editor
setup and what the build needs.

## Learn it

- **[Why Sword](docs/why.md)** — what the design buys against Go, C and Rust, and
  what it costs. Read this before the tour if you are deciding whether to care.
- **[A tour of Sword](docs/tour.md)** — start here. Every section is a program
  you can run, and they build on each other from hello world to parallel code.
- **[Sword by example](docs/examples/README.md)** — complete programs, one subject
  each: an allocator, a bytecode machine, a binary format, calling C.
- **[Concurrency](docs/concurrency.md)** — how tasks, partitions and the race
  checker fit together, and why they are shaped the way they are.
- **[HTTP](docs/http.md)** — writing a server and a client with `std/http`.
- **[Testing](docs/testing.md)** — `shield test`, and writing tests that sit
  beside the code.
- **[Reference](docs/reference.md)** — types, operators, keywords, standard
  library. For looking things up once you know the language.
- **[Grammar](docs/grammar.md)** — the syntax in EBNF, token by token, for
  writing a tool that reads Sword or settling what parses.

## What it looks like

Three commitments run through every design decision:

**Nothing allocates implicitly.** A function that touches the heap takes an
`Allocator` parameter. There is no other way, including in the standard
library, so a signature without one cannot allocate.

**A task's lifetime is a lexical scope.** `spawn` only exists inside `scope`,
and the closing brace is the join. That is what makes it safe to lend a task a
slice of memory the parent owns.

**Immutable by default.** `x := 1` cannot be reassigned; `mut x := 1` can. That
is not a style preference — immutable data crosses between tasks with no
synchronisation at all, which is what the race checker leans on.

## Status

Sword compiles to native code through LLVM and the language is usable, but it
is young. Working today: functions with variadic parameters and function values,
methods, interfaces with dynamic dispatch, generics over both functions and
structs, floating point, top-level constants, atomics, mutex-protected
`shared[T]` with wait and notify, structs, enums, slices, strings, pointers,
optionals, error unions, `switch` with completeness checking over an enum,
`defer`, packages, and the whole concurrency story with a work-stealing
scheduler behind it, channels included.

The standard library covers memory (`std/mem`), byte buffers (`std/bytes`),
byte-level strings (`std/strings`), UTF-8 and the width of a character
(`std/unicode`), generic containers and sorting (`std/collections`),
exact and compensated sums and elementary functions with the same bits on
every machine (`std/num`), random numbers that do not depend on the
number of threads (`std/rand`), parallel scan, filter, sort and minimum with
the same property (`std/par`), printing
and formatting (`std/io`, `std/fmt`), JSON (`std/json`), hashing, HMAC and
keys derived from passwords (`std/crypto`), hex and base64 (`std/encoding`),
arguments and environment (`std/os`), files, directories and
standard input (`std/fs`), running another program (`std/process`), C's
types by their C names (`std/c`), loading a library while the program runs
(`std/dl`), clocks and
durations (`std/time`), scheduler counters (`std/runtime`), TCP and UDP over IPv4 and IPv6 (`std/net`),
TLS (`std/tls`), HTTP/1.1 with keep-alive, routing, timeouts, static files and
HTTPS, server and client (`std/http`), and tests (`std/testing`, run by
`shield test`).

A task has its own stack, so a task waiting on a socket is put down rather than
holding a thread: the descriptor goes to a poller and the worker moves on.
Measured, on ten cores — 4000 concurrent keep-alive connections at 84 000
requests a second on twelve threads, and 2000 idle ones in 65 MiB.

TLS comes from OpenSSL, found at build time — `http.ListenTLS` for a server,
`http.Fetch("https://...")` for a client, and `http.ListenWith` for a server that
asks its clients for a certificate of their own. A build made on a machine without
OpenSSL says so (`tls.Available()`) instead of failing to link.

What comes next — packages, speed, smaller machines, and the compiler rewritten
in Sword — is in the [roadmap](docs/roadmap.md).

## Contributing

Bug reports, fixes, library code, documentation and editor support are all
welcome. A change to the language itself starts as an issue, where it is settled
before anybody writes it. [CONTRIBUTING.md](CONTRIBUTING.md) has how to build,
test and send a change, and everyone taking part follows the
[Code of Conduct](CODE_OF_CONDUCT.md).

## License

MIT.
