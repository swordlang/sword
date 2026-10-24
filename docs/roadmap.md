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
- `noalias` where the race checker has proved two pieces disjoint — only there,
  since a wrong one is undefined behaviour;
- a faster calling convention for functions nothing outside the program calls,
  and link-time optimisation with the runtime's hot paths;
- smaller binaries;
- `shield checks`, which lists every bounds check that is still there, line by
  line, so the ones that cost something in a loop can be looked at.

Every change is measured before and after, and the
numbers go out together with a comparison against C, C++, Rust and Go: the same
algorithms, idiomatic in each language, with versions and flags written down.

## 0.5 Polish — smaller machines

Sword assumes a 64-bit computer with an operating system. Polish takes that
apart:

- embedded Linux on 64-bit ARM, tested on real boards;
- `--target` for 32-bit and 16-bit machines, with layouts worked out for the
  target's pointer size instead of assumed;
- a standard library that works without an operating system. Which functions
  need one is inferred, the way `det` is, and using one on a bare-metal target
  is an error that says which call reached the system;
- a runtime for microcontrollers: tasks with small fixed stacks, cooperative,
  and interrupts.

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

## 0.6 — software that must not fail

What the compiler already refuses makes Sword a reasonable fit for software where
a failure costs more than a restart. 0.6 adds the guarantees that kind of work
asks for:

- a mode where nothing allocates once the program has started;
- the worst-case stack of every task, worked out by the compiler, with
  unbounded recursion refused;
- functions proved not to panic: no bounds check, overflow or `unreachable` on
  any path through them;
- a stricter subset in the spirit of MISRA, for code that has to follow one;
- contracts, `requires` and `ensures`, checked in debug builds and proved where
  the compiler can — and what a `requires` guarantees, the optimizer gets to use,
  so `requires n <= xs.len` takes the checks out of the loop below it;
- a function the compiler confirms has no bounds check left in it, for the
  kernel that has to stay fast;
- loops whose bound is proved, inferred as `det` is, which is what an estimate
  of the worst-case running time needs to start from.

Certification itself is a process, not a compiler, and stays out of scope.

## 0.7 — types that say more

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
- Generic methods.
- `link[Node, u16]`, a link stored as a narrow offset from where it sits instead
  of a full address: a tree of small nodes in half the memory, and a structure
  that means the same wherever it is copied, flash included.
- A growable container in the standard library that never moves what it holds,
  so a pointer to an element stays good as it grows.
- `mut` on an interface method's receiver, so that a call through an interface
  is checked like any other ([#78](https://github.com/swordlang/sword/issues/78)).

## 0.8 — compile time, and the rest of C

- Evaluating `const X = f(...)` at compile time when `f` is `det`: a CRC table or
  a sine table worked out once, by the compiler, and kept in flash. `det` already
  rules out everything compile time cannot do, so this needs no new keyword.
- The rest of C: structs passed by value, unions, `packed` and `align` on C
  types, bindings generated from a header with `shield bind`, and functions
  written in Sword that C can call.

## 0.9 — tools, and closing the language

- Debug information, and a backtrace when a program panics.
- `shield size` and `shield stack`: how much flash and how much stack each
  function costs, which is the first thing asked about a firmware image.
- Interrupts in simulation. `shield test -sim` already chooses the order tasks
  run in from a seed; it will also choose when each interrupt arrives, so a race
  between the main program and a handler — the kind of bug that shows up on the
  board once a week — happens again every time it is run with that seed.
- Simulated networking for `shield test -sim`.
- `std/term`, and a terminal UI framework built on it as a package of its own.

0.9 is the last version that changes the language before 1.0.

## 1.0 Edge — Sword in Sword

The compiler is written in C++ today. After 0.9 it is rewritten in Sword, in a
series of versions — 0.10, 0.11 and on — that change the compiler and not the
language. Each stage is checked against the C++ one on everything it can print —
the syntax tree, Sword's own IR, the LLVM IR — before the next is started.

Edge is the version whose compiler builds itself, twice, and gets the same binary
both times: a language that has stopped changing, and a compiler written in it.
The C++ compiler stays as the seed for building it on a new machine.

## Having a say

The order above is a plan and not a promise. If something here matters to what
you are building, or something missing does, open an issue that says what you
would write today and what you would rather write. That is how
cross-compilation and bare-metal targets got onto this page.
