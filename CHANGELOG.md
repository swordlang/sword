# Changes

Releases are numbered by [semantic versioning](https://semver.org): until 1.0,
a minor version may change the language, and each one says how. Each release is
also named for a step in making a blade, in the order they happen.

## 0.3.1 — Quench

*2026-10-30.* A patch to Quench: the race checker closes a hole in partitions
and stops refusing the parent's own uses, and two smaller mistakes are fixed.

### What changes for existing programs

One kind of program that compiled stops compiling, against what a patch
promises, because each of them is a data race the race checker was meant to
refuse. A piece of a `chunks` partition belongs to its task for one turn of the
loop that cut it; the checker took it to belong to it for good. Now refused:
nested `chunks` loops that spawn with a piece of each, a loop around the
`chunks` loop or inside it, two spawns given one piece in a turn, and the parent
writing a piece beside its task (#135). A spawn in each branch of an `if` and
the parent reading `part.len` are still fine. Everything else that compiled
still does.

### Fixes

- Inside a `scope`, the parent reading a local of its own and then writing it
  was refused as a race. Its own uses run one after another and race only with
  its tasks (#136).
- Lending one counter or struct to every task of a loop came with a note to
  walk it with `chunks`, which only a slice has. The note now says `atomic[T]`
  for an integer or a bool, `shared[T]` with `lock` for anything else, and
  `a[..].chunks(n)` for an array (#137).
- A package constant as an array length in a generic struct's field, a generic
  method's parameter or a generic function's parameter was "not an integer
  constant" (#138).

## 0.3.0 — Quench

*2026-10-24.* Quenching sets the steel: this release lets a program be built
from packages other people wrote, fetched from git and pinned by hash, and
closes the ways read-only memory could still be written.

### What changes for existing programs

- **A copy of read-only memory cannot write it.** `mut ys := xs`, where `xs` was
  lent read-only, used to write through to it, and `&x` passed an immutable `x`
  to a `mut` parameter. The copy is still legal and can be reassigned; writing
  through it is refused. What a call writes through its arguments is worked out
  for every function, so a method that only moves its own position can still be
  called on a parser that holds its input read-only.
- **The race checker sees one memory under two names.** `ys := xs` with both
  spawned to write, two pointers taken from one variable, a task given
  `p := &n` while its parent writes `n`: each compiled and each is a data race.
  The error names the memory and the name it was reached through.
- **`~x` is the complement**, at the width of `x`. `a ~ b` used to be a xor;
  it no longer compiles into anything else. `->` is no longer a token.
- **A statement on its own has to call, send or receive.** `total + i`, a bare
  name or a comparison computed a value and dropped it, which is almost always
  a `+=` or a `=` that lost a character.
- A struct field takes no `mut` and no `...`, and `defer` takes no declaration;
  all three were accepted and meant nothing.
- **Tests and writers ask for less `try`.** `t.Equal`, `Same`, `Check`, `Fail`,
  `Failf` and `Logf` record and carry on, so one run reports every mismatch;
  **`try t.Must`** compares and ends the test. Writes to a `json.Writer` and to
  an `http.Response` keep their first error and do nothing after it:
  `w.Err()` and `res.Err()` report it, and the server answers 500 for a failed
  response, or closes the connection when the head has gone. A `try` left in
  front of one of these no longer compiles.
- `Allocator.Release` and the `Mux`'s `Handle` and its shortcuts take what they
  write as `mut`.
- **Two imports that give one name are an error**, where the first used to win
  silently. `import name "path"` gives one of them another.
- **The standard library's errors keep the same codes in every program**, 1 to
  75, whether the program imports their package or not, and a program's own
  come after them. Exit statuses of fallible programs change accordingly, and an
  error code past 255 exits with 255: a code of 256 used to leave with 0.

### Packages

- **`sword.mod`** names a module and what it requires; `replace` points a
  dependency at a local directory. Versions are git tags, chosen by minimal
  version selection; a branch or a commit with no tag gets a version of its
  own that sorts before every release.
- **A build fetches** what the cache (`~/.sword`) is missing, over git, and
  checks each version against **`sword.sum`**, which holds a hash of the files
  the compiler reads. A missing line is added; `--locked` refuses to.
- **`shield get path[@version]`** adds or moves a dependency, `@none` removes
  it, and a bare path takes the newest stable tag. **`shield vendor`** copies
  everything into `vendor/` for building without a network.
- Fetching never runs a package's code: a Sword package has no build script.
- **`import name "path"`** gives an imported package another name.

### Language

- **`lock read`** lets any number of readers in at once and no writer; inside it
  the name is an immutable pointer. A writer waiting keeps new readers out.
- **`if lock`** takes a guard only when it is free, and runs `else` when it is
  not. Both work for either kind of lock.
- **`if mut x := opt`** and **`for mut x := opt`** bind the unwrapped value as
  writable.
- **`extern blocking func`** declares a C function that may block: each call
  runs on the io pool while the task is put down, so it does not hold a
  scheduler thread, and the errno it leaves comes back with the answer. It costs
  about 5 µs a call, so it is for calls that wait, not ones that compute.
- Writing a closure is answered with "Sword has no closures" and what to write
  instead, rather than a parse error about function types.

### Tools

- **`shield new`** starts a module: a program, or a library with `--lib`, with
  a test and `git init`.
- **`shield doc`** prints what a package exports, or one type or method, from
  the comments above the declarations.
- **`shield --split`** compiles each package to an object of its own and keeps
  it in `~/.sword/build`, so only what changed goes through LLVM. Rebuilding a
  small HTTP server after editing only its own package took 86 ms instead of
  235 here. It is not the default: inlining stops at the edge of a package, and
  a hot loop around a small call into the standard library can run several
  times slower. Objects unused for thirty days go by themselves; **`shield
  clean`** removes them all.

## 0.2.4 — Fold

*2026-10-21.* A patch to Fold for programs that talk to C: C's types by their
C names, and the errno a failing C function leaves behind.

### Standard library

- **`std/c`** names C's types the way a header does — `c.Int`, `c.Long`,
  `c.SizeT`, `c.SSizeT`, `c.Double`, `c.Bool`, `c.String` for a
  `const char *`, `c.Ptr` for a `void *`, and the rest — so a declaration
  copied from a header reads like the original. Each is an alias, so a `c.Int`
  is an `i32` and passes for one. `c.Char` is `u8`, for text; a plain `char`
  passed by value takes `c.SChar` or `c.UChar`, since whether it is signed is
  the platform's choice.
- **`os.Errno`** reads the errno a C function just left, and
  **`os.ClearErrno`** zeroes it for `strtol` and its family, which say they
  failed only through errno. errno belongs to the thread, so it is read
  straight after the call, before anything that could wait.

### Documentation

- The Neovim section of the editors' README, with a fix for installing
  `swordls` under Lua's conventions.

Nothing that compiled before stops compiling.

## 0.2.3 — Fold

*2026-10-20.* A hotfix to Fold for programs that talk to C: strings cross
between the two without each program writing its own conversion.

### Standard library

- **`strings.FromC`** reads a C string up to its zero, and **`strings.FromCMax`**
  also stops at a limit, for a `char[N]` field the text fills. Both answer with
  a view of the C memory and copy nothing.
- **`strings.ToC`** writes a string and its zero into a buffer of the caller's
  for a C function that wants a `const char *`, and refuses a string holding a
  zero, which C would read as a different, shorter one.

Nothing that compiled before stops compiling.

## 0.2.2 — Fold

*2026-10-20.* A patch to Fold: what a server needs to authenticate, sign, stamp
and cache, a second name for a type, the grammar written down, and three bugs
in the runtime and its tests that had been showing up as flakes.

### What changes for existing programs

Nothing that compiled before stops compiling. `type` is not a keyword: it is
special only at the start of a top-level declaration followed by a name, so a
field or a variable called `type` is still a name.

### The language

- **`type Name = T`** gives a type a second name. A `Name` is a `T` — values
  pass between them without a conversion — and an alias of a struct can write
  a literal or declare a method. It takes no type parameters, though it may
  name an instance: `type Set = collections.Map[string, bool]`. The language
  server and the editors' own colouring know it.
- **An array's length may be another package's constant**:
  `[crypto.Sha256Size]u8`.
- **[The grammar](docs/grammar.md)**, in EBNF: the lexical rules, including when
  a newline ends a statement, and every production of the syntax.

### Standard library

- **`std/crypto`**: HMAC-SHA-256, `crypto.Equal` (a comparison that takes the
  same time wherever the bytes differ), PBKDF2-HMAC-SHA-256, and
  `crypto.Random`, bytes from the kernel's random source. Under
  `SWORD_SIM_SEED` the random bytes come from the seed, so a simulated run that
  makes a token makes the same one again.
- **`std/encoding`**: hex, base64 and base64url into the caller's buffer.
  Decoding is strict, so each byte string has one spelling.
- **`std/time`**: `Rfc3339`, `HttpDate`, `ParseRfc3339` and `ParseHttpDate`,
  which reads all three forms RFC 9110 requires a recipient to accept.
- **`std/http`**: static files carry `Last-Modified` and a weak `ETag`, and
  `Files` answers `If-None-Match` and `If-Modified-Since` with 304.

Nothing in any of them allocates.

### Fixes

- A dial whose connect failed closed its socket outside the poller; on macOS
  that is the close beside kqueue that can hang in the kernel until reboot
  (#41).
- `tests/dialreuse.sword` lost dials on macOS because it could fill the listen
  queue, which there answers with a reset (#53).
- Under ThreadSanitizer, a signal could be held in a task that parked and never
  came back; `graceful` hung about once in three hundred runs (#48).
- `shield test -sim` on a standard library package warned about the runtime's
  own calls.

## 0.2.1 — Fold

*2026-10-20.* A patch to Fold that finishes the way out to C: a library named in the
source and linked either way, a C function as a value, and a library loaded
while the program runs.

### What changes for existing programs

- **The name of an `extern` function, used as a value, is an `extern func`**,
  not a `func`. Code that handed one where a `func` was expected no longer
  compiles; a `func` can only point at Sword code, which is what lets `det`
  follow a call through one.
- **An integer narrower than 32 bits reaches C widened.** On Apple's arm64 an
  `extern` taking an `int8_t`, an `int16_t` or a `bool` read whatever the upper
  bits of the register held.
- **Every function but `main` is internal to the program.** LLVM drops what
  nothing calls: on an Apple M4 a hello world went from 145 376 to 119 400 bytes
  and the HTTP test from 281 752 to 168 552, and building the latter takes about
  half the time. A program that uses part of a package no longer references the
  C symbols the rest of it needs.

### C

- **`extern "z" func ...`** names the library a function comes from, and it is
  linked without being mentioned on the command line. pkg-config is asked where
  it is when the machine has pkg-config.
- **`shield --static`** links each declared library from its archive and refuses
  to build without one. The system's own libraries stay dynamic.
- **`extern weak func ...`** — a function the program can run without. Its name
  is an `?extern func`, to be checked before it is called.
- **`extern func(T) R`** — a C function as a value. A Sword function converts to
  one, which is how `qsort` gets a comparison; an address converts to one, which
  is how a loader's answer gets a type.
- **`std/dl`** opens a library while the program runs, finds functions in it,
  and keeps the system's own account of what went wrong in `Loader.Why()`.
- **`--emit-link`** prints the rest of the link line, for a build that links by
  its own means.

### Other machines

- **`--target=<triple>`** builds for another 64-bit, little-endian machine,
  against a runtime built for it with `make TARGET=<triple>`. **`-c`** stops at
  the object file and needs nothing but clang.

### Fixes

- `shield test` on a program — a package with a `main` — failed in clang every
  time (#4).
- A function can return a function value; the parser read the result type as
  the start of the body.
- Two copies of the test suite on one machine no longer overwrite each other's
  certificates, files and test binaries (#36), and `shield test` no longer puts
  a binary beside the package for the length of a run.
- The value of `shield test -p` reached the shell unquoted.
- `-sim` warns about a call through an `extern func`, as it does about a call to
  an `extern` declaration.

## 0.2.0 — Fold

*2026-09-28.* Folding the steel is how the flaws are worked out of it: this
release makes concurrency bugs you can reproduce, and numbers you can see.

### What changes for existing programs

- **`{}` prints a float as the shortest text that reads back as the same
  double.** It printed six places: `1e-9` came out as `0.0` and `1.0 / 3.0` as
  `0.333333`. Now `0.1` is `0.1`, `0.1 + 0.2` is `0.30000000000000004`, `1e-9`
  is `1e-9`, and whole numbers keep their `.0`. Plain from 1e-4 to 1e16, a
  mantissa and a power of ten outside.
- **`{.N}` is exact and always fixed point.** The digits are the double's own,
  rounded once with ties to even, as C's `printf` does: `{.20}` of `0.1` is
  `0.10000000000000000555`. A large number prints all its digits where it used
  to switch to exponent form.
- Negative zero prints with its sign.
- `std/json` reads numbers as the nearest double; `0.3` used to come out one
  unit high.

### Simulation

- **`shield test -sim`** runs each test on one thread with every scheduling
  choice drawn from a seed, once per seed (100 by default, `-seeds N`), and a
  failure comes with the seed and the command that makes it happen again
  (`-seed S`). A deadlock is reported as one instead of hanging. Time is
  virtual — it jumps to the next deadline when every task waits on it — and the
  wall clock starts at 2000-01-01 00:00:00 UTC.
- **`SWORD_SIM_SEED=n`** runs any program the same way. `--sim` also switches
  tasks at every atomic operation; without it the generated code is unchanged.
- Tests that open a socket are skipped under `-sim` until the network is
  simulated. A call out of the language gets a compiler warning under the
  simulator, which cannot replay it.

### Numbers

- **`strings.ParseF64`**: the double nearest the text, a tie going to the even
  one. What `{}` prints reads back as the same double.
- **`num.Sin` and `num.Cos` reduce ordinary arguments the way fdlibm does**:
  the same bits as before for every argument checked, and a sine of an argument
  in [-1000, 1000] went from about 174 ns to 11.7 ns on an Apple M4.

### Fixes

- A task can wait — for a timer, a socket, a `Wait` — inside a `lock`. On one
  thread that used to be reported as a deadlock that was not one, and a
  contended lock now puts the task down instead of spinning its thread.
- A task at the end of a `scope` parks instead of running the scope's tasks on
  its own stack, which could leave a task waiting for one buried beneath it.
- A socket made or accepted just after another was closed could read the old
  descriptor's generation and fail its first wait: `net.Dial` reported a failure
  over a connection that had been made. The TLS test lost one connection in a
  hundred runs on Linux this way.

### How changes get in

Every pull request now runs, on Linux and macOS: the whole suite with TLS and
without and at 1 to 16 threads, AddressSanitizer and ThreadSanitizer
(`tests/sanitize.sh`), every example program in the documentation
(`tests/docs.sh`), a check of commit subjects, a review of new dependencies,
and CodeQL. There are issue and pull request templates and a security policy.

## 0.1.0 — Tamahagane

*2026-09-27.* The raw steel: the first release, and everything the language is
built from.

### The language

- Go's syntax, no garbage collector, and nothing that allocates without being
  handed an `Allocator` — a signature without one cannot touch the heap.
- Immutable by default; `mut` belongs to the binding, not the type.
- Errors are values that cannot be ignored: `!T`, `try`, `catch`, and errors
  declared with the sentence they say. Optionals are `?T` with `orelse`.
- Structs, enums with exhaustive `switch`, interfaces, generics, slices that
  know their length, `defer`, packages with the uppercase rule for what is
  exported.
- Bounds and integer overflow checked in the default build; `--mode=fast` and
  `--mode=small` for when they have been paid for.
- Two constant strings join with `+` at compile time; at run time,
  `strings.Concat` takes the allocator the result goes in.

### Concurrency

- `scope` and `spawn`: a task cannot outlive the block that started it, and
  everything it can reach is on the line that starts it.
- The compiler refuses a program where two tasks can touch the same memory,
  unless both only read or the pieces come from one `chunks` partition.
  `atomic[T]` and `shared[T]` are the two ways to share on purpose.
- Every task has its own stack, and a wait on a socket, a timer, a file or a
  name lookup puts the task down, not the thread. Measured on ten cores: 4000
  concurrent keep-alive connections at 84 000 requests a second on 12 threads,
  and 2000 idle connections in 65 MiB.
- `chan[T]`, `<-`, `close` and `select`.
- `parallel for` spreads a loop over every core, with `reduce` for the
  accumulators.

### The same answer, however many threads

- `det` on a function is a claim the compiler checks: what it answers depends on
  its arguments and nothing else. A `scope`, a `parallel for`, and a pipeline of
  tasks joined by channels with one sender and one receiver each are allowed
  inside it; an atomic, a lock, the clock or an address read as a number are
  not, and the error walks the chain of calls to the line that broke it.
- A reduction answers the same bits at any thread count, floating point
  included, and a scope reports the error of the earliest task that failed.
- `std/rand`: random numbers where the i-th depends on the seed and i (Philox).
- `std/par`: a prefix sum, a filter that keeps the order, the position of the
  smallest element, and a stable sort, all under the same rule.
- `std/num`: `Sqrt`, `Exp`, `Log`, `Sin` and `Cos` with the same bits on every
  machine and within one unit in the last place, and an exact sum that rounds
  once.
- Every change runs the whole test suite at 1, 2, 3, 4, 8 and 16 threads and
  requires the same bytes out of every program.

### The standard library

`std/mem`, `std/io`, `std/fmt`, `std/bytes`, `std/strings`, `std/unicode`,
`std/collections`, `std/json`, `std/crypto`, `std/num`, `std/rand`, `std/par`,
`std/os`, `std/fs`, `std/process`, `std/time`, `std/runtime`, `std/net` (TCP and
UDP, IPv4 and IPv6), `std/tls` (over OpenSSL, optional at build time),
`std/http` (HTTP/1.1 server and client, routing, keep-alive, streaming, static
files, timeouts and deadlines, HTTPS), `std/chan` and `std/testing` (tests and
benchmarks, run by `shield test`, in parallel by default).

### Tools

- `shield`, the compiler: `.sword` to LLVM IR, assembled and linked by `clang`.
- `swordls`, the language server, built on the same front end, so a diagnostic
  in the editor is the one the compiler gives.
- Editor support for VS Code, vim, Neovim and Emacs.

### Where it runs

macOS and Linux, on arm64 and x86-64, each change tested on both systems with
TLS and without. No Windows.

### What it does not do yet

No package manager. Use-after-free is not tracked: tasks cannot outlive what
they borrow, but freeing something and reading it is yours to avoid. Tens of
thousands of concurrent tasks, not millions. The HTTP client keeps one
connection per host rather than a pool.
