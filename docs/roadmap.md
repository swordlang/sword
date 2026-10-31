# Roadmap

Where Sword is going, in the order it is likely to get there. Versions stay
below 1.0 until the language stops changing under people's feet, and each one
is named for a step in making a blade: Tamahagane, Fold and Quench are out,
Temper is next, and the last is Edge, at 1.0.

Nothing here has a date. A version ships when what it promises works and has
been measured, and the [changelog](../CHANGELOG.md) says what each one actually
did. A minor version may still change the language before 1.0, and when it
does, its changelog entry says how.

## What does not move

Some things are not on the roadmap because they are not going to change. They
are why the language exists:

- nothing allocates without an `Allocator` being passed in;
- a task lives inside the `scope` that started it, and the closing brace is the
  join;
- bindings are immutable unless they say `mut`;
- a program with a data race does not compile;
- a function value is a function declared at the top of a file, with nothing
  captured, and `spawn` takes a call and nothing else, so that everything a task
  can reach is written on one line.

A proposal that needs one of these to bend is a different language.

Neither is the way the language grows. A new feature is a word you can read aloud
— `scope`, `orelse`, `shared[T]` — rather than a symbol to look up, and what the
compiler can work out for itself, it works out: `det` is inferred for every
function and checked only where somebody writes it, and that is the pattern for
everything below.

## 0.3 Quench — packages

Before Quench a program could use the standard library and code sitting next
to it, and nothing else. Quench added packages, fetched from git:

```
module github.com/pedro/site
sword 0.3

require github.com/ana/sqlite v1.4.0
```

`shield get` adds a dependency; a build fetches whatever `sword.mod` asks for
and the cache does not have yet. Versions are git tags, chosen by minimal version
selection: the newest version anybody asked for, never the newest that exists.
`sword.sum` holds a hash of each version's contents, so a tag moved to other code
is caught. `shield vendor` copies everything into the project for building
without a network, and `replace` points a dependency at a local directory while
both are being worked on. There is no central registry, and fetching a package
never runs any of its code: a Sword package has no build script.

A package is compiled with the program, from source, the way the standard
library already is. That is what lets the race checker and `det` see through
it.

Quench also changed some things that compiled under Fold:

- a copy of a slice or pointer lent read-only cannot be written through, and the
  race checker sees two names for the same memory as the same memory;
- `lock read` for any number of readers at once, and `if lock` for not waiting;
- `~x` is the complement, and a statement on its own has to call, send or
  receive;
- tests check with `t.Equal` and carry on, `try t.Must` to stop, and neither the
  JSON writer nor an HTTP response asks for `try` at every write.

It also brought `extern blocking func`, for a C call that blocks without
holding a scheduler thread; `import name "path"`, for two packages that would
otherwise have one name; `shield new` and `shield doc`; and `shield --split`,
which keeps each package's object between builds for a quicker edit-and-run
cycle.

## 0.4 Temper — speed

Sword gets correct code out of LLVM, but not yet fast code. The compiler emits
bounds checks it could prove away, calls everything with the C convention, and
never lets LLVM see into the runtime. Temper is about that:

- drop the bounds checks the language already proves, such as `xs[i]` inside
  `for i in 0..xs.len` over a slice that cannot change;
- settle the overflow checks of a loop that does nothing but add once, after
  it, instead of on every addition, with the same panic on the same line;
- `noalias` where the race checker has proved two pieces disjoint — only there,
  since a wrong one is undefined behaviour;
- a faster calling convention for functions nothing outside the program calls,
  and link-time optimisation with the runtime's hot paths;
- smaller binaries: no TLS library in a program that does not import
  `std/tls`, and nothing in the binary that nothing calls;
- `shield checks`, which lists every bounds check that is still there, line by
  line, so the ones that cost something in a loop can be looked at;
- `runtime.SetThreads(n)`, so a program says how many workers it runs on
  instead of leaving it to whoever sets `SWORD_THREADS`. It holds until the
  first task starts, which is when the workers are made, and returns an error
  after that. The variable stays, as the default the function overrides;
- a runtime that needs no C++ library. On Linux, loading `libstdc++` is half of
  what it takes a program that does nothing to start, and a binary that needs
  it does not run on an image without it. The runtime keeps its C++ but stops
  using that language's standard library, so a program asks for the C library
  and nothing else.

Every change is measured before and after, and the
numbers go out together with a comparison against C, C++, Rust and Go: the same
algorithms, idiomatic in each language, with versions and flags written down.

One change in Temper is not about speed. A method called through an interface
could write through a value lent read-only, because an interface did not say
which of its methods write; the race checker trusted the signature and missed
it. Interface methods now say so with `mut`, and a call through an interface is
checked like any other
([#78](https://github.com/swordlang/sword/issues/78)). Programs that call a
writing method of `mem.Allocator` or `fmt.Sink` on a value that is not `mut`
stop compiling, and the message says what to add.

## 0.5 Polish — more machines

Sword assumes a 64-bit computer running a Unix. Polish takes that apart:

- Windows, on x86-64 and 64-bit ARM, tested on every change next to Linux and
  macOS. The runtime waits for the network through I/O completion ports there,
  the way it uses kqueue and epoll elsewhere, and a program written for one
  compiles for the others unchanged;
- on Linux, the runtime waits through `io_uring` instead of epoll and a pool of
  threads for files; a program does not change, only how long it waits;
- embedded Linux on 64-bit ARM, tested on real boards;
- WebAssembly, `wasm32` with WASI, for plugins run in a sandbox, code at the
  edge of a network and programs in a browser. Tasks there are cooperative on
  one thread, as on a microcontroller: `scope`, `spawn` and channels work, and a
  `parallel for` runs its pieces one after another, with the same bits, since
  the cut never depended on the threads;
- `--target` for 32-bit and 16-bit machines, with layouts worked out for the
  target's pointer size instead of assumed;
- a standard library that works without an operating system. Which functions
  need one is inferred, the way `det` is, and using one on a bare-metal target
  is an error that says which call reached the system;
- a runtime for microcontrollers: tasks with small fixed stacks, cooperative,
  and interrupts. A program that never starts a task carries no scheduler at
  all.

A microcontroller is programmed by reading and writing registers at fixed
addresses and by answering interrupts, so Polish brings the language for that
too, rather than leaving every register to a function written in C:

- `mmio[u32](0x4002_0014)`, a register reached only through its methods, the way
  an `atomic[T]` is, so the compiler never reorders or drops an access;
- `packed struct`, with fields as wide as the bits they hold — `mode u2` — for
  registers and wire formats that are fields of bits;
- `interrupt func`, checked the way `det` is: a handler that allocates, takes a
  lock or waits does not compile, and the vector table is built from the
  handlers themselves;
- `asm` for the few instructions nothing else can say, such as `wfi`;
- placing code and data in a named section, and constants that are whole arrays
  and structs, kept in flash instead of copied into RAM.

On the other side of the scale, the HTTP client keeps a pool of connections per
host instead of one, and can send a body in chunks as the server already does.

## 0.6 — graphics processors

A GPU runs thousands of threads over one space of indices, has memory the CPU
cannot read, and allocates nothing while a kernel runs. Those are nearly Sword's
rules already — nothing allocated behind your back, no closures, data that
arrives through slices you pass — so 0.6 makes the GPU part of the language,
rather than a library that loads kernels written in another one:

- `gpu for i in 0..n { ... }`, the sibling of `parallel for`: the body runs once
  per index on the device, and the race checker holds it to the same rule, each
  index writing only its own element. `reduce(+: total)` works the same way;
- a reduction on the GPU follows an order the language fixes, the way a float
  sum in a `parallel for` already does, so a `det` function gives the same bits
  on the GPU as on the CPU, whatever the card. A floating-point atomic, whose
  order the hardware picks, is outside `det`;
- memory on the device is a `gpu.Buffer[T]`, never a slice. A slice can be read
  anywhere and memory on the card cannot, so a buffer is filled and read back by
  explicit calls and indexed only in code that runs on the GPU;
- `kernel func`, for what `gpu for` hides — blocks, memory shared within a block,
  barriers — with the race checker extended to the threads of a block;
- waiting for the GPU parks the task, the way waiting for the network does, so a
  server goes on answering while its kernels run;
- `f16` and `bf16`, on the CPU as well as on the GPU;
- `simd[f32, 8]`, a vector whose element and width the program writes, for the
  CPU as for the GPU. Its operators act element by element, adding up its
  elements follows an order the language fixes, and a machine with narrower
  vectors uses several of them with the same bits — so explicit SIMD stays
  inside `det`, where intrinsics written for one processor do not;
- a machine without the GPU a program asks for is an error the program handles:
  `gpu.Open()` returns `!gpu.Device`. Nothing runs on the CPU behind its back,
  since that would hide the very difference the program was written for.

The first backend is NVIDIA's, through the PTX backend LLVM already has, which
covers servers and embedded boards such as Jetson.

## 0.7 — software that must not fail

What the compiler already refuses makes Sword a reasonable fit for software where
a failure costs more than a restart. 0.7 adds the guarantees that kind of work
asks for:

- a mode where nothing allocates once the program has started;
- the worst-case stack of every task, worked out by the compiler, with
  unbounded recursion refused;
- functions proved not to panic: no bounds check, overflow or `unreachable` on
  any path through them;
- a stricter subset in the spirit of MISRA, for code that has to follow one;
- packages that say what they reach. Which functions reach the network, files,
  other processes, the clock or C is inferred, the way `det` is, and `sword.mod`
  records it for every dependency. A new version that starts reaching something
  the one before did not does not build until somebody accepts it, and the
  message names the function that reached it. A package runs nothing when it is
  fetched, so what it reaches is what it can do;
- contracts, `requires` and `ensures`, checked in debug builds and proved where
  the compiler can — and what a `requires` guarantees, the optimizer gets to use,
  so `requires n <= xs.len` takes the checks out of the loop below it;
- a function the compiler confirms has no bounds check left in it, for the
  kernel that has to stay fast;
- loops whose bound is proved, inferred as `det` is, which is what an estimate
  of the worst-case running time needs to start from.

Certification itself is a process, not a compiler, and stays out of scope.

0.7 also brings the second GPU backend, Vulkan and SPIR-V, which reaches AMD
and Intel cards and the GPUs inside embedded chips.

## 0.8 — types that say more

- Tagged unions with an exhaustive `switch` over them: a message that is one of
  several shapes, a state machine, a syntax tree. A function may also be written
  once per variant — `area(s Shape.Circle)`, `area(s Shape.Rect)` — and the
  compiler checks that every variant has one. That is the only overloading Sword
  will have: a name used as a value always means one function.
- Blocks: a function written in place and handed to a call, which sees the locals
  around it and is gone when the call returns.

  ```
  collections.Sort(people) { a, b => a.age < b.age }
  evens := par.Keep(xs, &arena) { x => x % 2 == 0 }
  ```

  A block is not a closure. It cannot be stored, returned or spawned, so a
  function value is still one function and a task still reaches only what its
  `spawn` line says; the race checker reads the block as the code it is written
  in. It compiles to the loop it looks like.
- Distinct types. `type Millis = distinct u32` is a new type with `u32`'s
  representation, so milliseconds and microseconds stop mixing by accident; an
  alias, as today, stays the same type under another name.
- Ranges. `type Percent = u8 in 0..100` is checked where a value comes in, the
  way Ada does it, and the bounds checker can lean on what it knows.
- Integers with a byte order, `u32be` and `u32le`, for reading a packet or a
  file format without converting by hand.
- `secret[T]`, a value that may not decide an `if`, be an index, be divided or
  be printed, and leaves the secret only through `.Reveal()`. A function it
  passes through is checked without being marked, which is the constant-time
  discipline cryptographic code needs, done by the compiler instead of by
  review — on a server holding keys, and on a board an attacker can put an
  oscilloscope on.
- Generic methods.
- `link[Node, u16]`, a link stored as a narrow offset from where it sits instead
  of a full address: a tree of small nodes in half the memory, and a structure
  that means the same wherever it is copied, flash included.
- A growable container in the standard library that never moves what it holds,
  so a pointer to an element stays good as it grows.

## 0.9 — compile time, and the rest of C

- Evaluating `const X = f(...)` at compile time when `f` is `det`: a CRC table or
  a sine table worked out once, by the compiler, and kept in flash. `det` already
  rules out everything compile time cannot do, so this needs no new keyword.
- `assert` at the level of a package, worked out by the compiler the same way:
  `assert sizeof[Header]() == 16, "the header is 16 bytes on the wire"` stops
  the build when it is false, which is what pins the layout of a wire format or
  a hardware register.
- Files in the binary: `const page = embed("static/index.html")` is a read-only
  `[]u8` kept in the executable, in flash on a microcontroller, and
  `embed("static/")` is a table from names to bytes. A server becomes one file
  to copy, and a board keeps its fonts and tables out of RAM. The path is
  relative to the source file and cannot leave the module, so a package embeds
  only what is its own.
- The rest of C: structs passed by value, unions, `packed` and `align` on C
  types, bindings generated from a header with `shield bind`, and functions
  written in Sword that C can call.
- C inside a package. Every `.c` file in a package's directory is compiled by
  the same clang the compiler already runs, with flags from a `cflags` line in
  `sword.mod`, so a package can carry SQLite as source with no build script and
  no `make`. A package that does reaches C, and says so with the rest of what it
  reaches.
- `shield build --lib`, a static and a shared library with a C header for the
  functions C can call, so that a program in Python, Go or Rust can use Sword
  through the door they all have, and Sword can come into an existing project a
  piece at a time. The runtime goes with it and starts on the first call, so
  tasks and scopes work inside the library.

## 0.10 — tools, and closing the language

- Debug information, and a backtrace when a program panics.
- `shield fmt`: one format and no options, the one the editors already indent
  to, so every Sword program looks like every other. The language server
  formats on save, and `shield fmt -check` fails a build that is not formatted.
- Coverage, `shield test -cover`: the lines and branches no test ran and, with
  `-sim`, which orderings between tasks the seeds have visited.
- `shield profile`, which samples where the time goes and adds it up by task
  and by scope, named after the function that started each task — on a server
  the question is which request is slow, and a pool thread does not answer it.
  It also writes pprof, for the tools that read it.
- Reproducible builds: the same source and the same compiler give the same
  binary, byte for byte, on any machine — no date, no absolute path and no file
  order in what comes out. CI builds every change twice, in two places, and
  compares. With the packages of 0.7 saying what they reach, it can be shown
  that what runs in production came from the source that was reviewed.
- `shield size` and `shield stack`: how much flash and how much stack each
  function costs, which is the first thing asked about a firmware image.
- Interrupts in simulation. `shield test -sim` already chooses the order tasks
  run in from a seed; it will also choose when each interrupt arrives, so a race
  between the main program and a handler — the kind of bug that shows up on the
  board once a week — happens again every time it is run with that seed.
- Simulated networking for `shield test -sim`.
- Recording a run and replaying it. A program with no data race computes what
  it computes from what reaches it from outside — the network, the clock — and
  from the order in which its tasks took locks, values from channels and turns
  at an atomic. Any program started with `SWORD_RECORD=file` writes those down,
  and `shield replay file` runs it again the same way, under a debugger if need
  be: the bug a server shows once a week, on the developer's machine. Left off,
  it costs a test of one bit where input comes in; what it costs on is measured
  before it is promised.
- Fuzzing, `func FuzzX(mut f *testing.F)`, run by `shield test -fuzz`. It varies
  both the input and the seed that orders the tasks, and a failure is kept as
  that pair, which fails every time it runs.
- `std/term`, and a terminal UI framework built on it as a package of its own.

0.10 is the last version that changes the language before 1.0.

## 1.0 Edge — Sword in Sword

The compiler is written in C++ today. After 0.10 it is rewritten in Sword, in a
series of versions — 0.11, 0.12 and on — that change the compiler and not the
language. Each stage is checked against the C++ one on everything it can print —
the syntax tree, Sword's own IR, the LLVM IR — before the next is started.

The runtime follows the compiler. It is C++ and a page of assembly today: the
scheduler, the tasks' stacks, the poller, channels and the pool for calls that
block, about five thousand lines. Written in Sword, it is compiled with the
program and inlined into it where that pays, and no program links a C++
library. First the language has to be able to say what a scheduler says:
atomics with orderings weaker than sequential consistency, a thread's own
variables, and a way to write the few structures that several threads change
by design, such as a work-stealing deque, without switching off the race
checker for the code around them. How it says each of those is decided when
the work gets there.

Edge is the version whose compiler builds itself, twice, and gets the same binary
both times: a language that has stopped changing, and a compiler written in it.
The C++ compiler stays as the seed for building it on a new machine.

The [reference](reference.md) stops describing the language and becomes its
definition, with a suite of conformance tests beside it — programs and the
output or the error each one must give — that any compiler of Sword has to
pass. The compiler written in Sword is the first one judged by it.

## Having a say

The order above is a plan and not a promise. If something here matters to what
you are building, or something missing does, open an issue that says what you
would write today and what you would rather write. That is how
cross-compilation and bare-metal targets got onto this page.
