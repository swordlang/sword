# Reference

For looking things up. [The tour](tour.md) is the place to learn from.

## Types

| | |
|---|---|
| `i8 i16 i32 i64` | signed integers |
| `u8 u16 u32 u64` | unsigned integers |
| `int` `uint` | word-sized, 64 bits |
| `f32 f64` | floating point |
| `bool` `void` | |
| `string` | immutable byte slice, two words |
| `error` | an error code, 16 bits |
| `*T` | pointer, never null |
| `?T` | optional; over a pointer it is the pointer, null meaning absent |
| `!T` | may fail: a `{ code, value }` pair |
| `[]T` | slice: pointer and length, two words |
| `[N]T` | array, a value |
| `[*]T` | raw pointer, no length, for FFI |
| `struct` `interface` | |
| `enum` | a named set of integers, with its own type |
| `func(T, mut U) R` | a function value: one word, no capture |
| `extern func(T) R` | a C function: one word, called the way C calls |
| `atomic[T]` | an integer or bool many tasks may write at once |
| `shared[T]` | any value, behind a mutex; `lock` is the only way in |
| `any` | a boxed value, what a `...any` parameter gathers |

`?*T`, `?[*]T`, `?func(...)` and `?extern func(...)` cost one word. Over anything else, `?T` is the
value with a flag beside it.

### String literals

Bytes between double quotes. The escapes, and there are no others:

| | |
|---|---|
| `\n` `\t` `\r` | newline, tab, carriage return |
| `\0` | a zero byte |
| `\e` | ESC, the same byte as `\x1b` |
| `\xNN` | the byte NN, two hex digits, either case |
| `\\` `\"` `\'` | the character itself |

`\xNN` is what a terminal sequence is written with, and it is what lets one be
a constant:

```sword
const ClearScreen = "\x1b[2J"
const Home = "\e[H"
```

## Declarations

```sword
x := expr              // immutable, type inferred
mut x := expr          // mutable
x T = expr             // immutable, type written
mut x T = expr         // mutable, type written

const Name = expr      // package level; folded at compile time
```

A constant's initialiser has to be literals, operators and other constants.
Declaration order does not matter, and a cycle is an error. A constant that is
an integer may be used as an array length. Two constant strings join with `+`,
at compile time; at run time joining allocates, and that is `strings.Concat`.

```sword
func name(a T, mut b U) R { ... }
func name(a, b T) R { ... }        // one type covers the names before it
func name(a T, rest ...U) R { ... } // the last one gathers what is left
func name[T, U: Constraint](a T) R { ... }
func (r *T) Method() R { ... }
func (mut r *T) Method() R { ... }
det func name(a T) R { ... }        // checked: the answer is a function of a
extern func c_name(a T) R          // C ABI, name unchanged
extern func c_name(a T, ...) R     // C's own variadic: ioctl, fcntl, printf
extern "z" func c_name(a T) R      // and it comes from libz
extern weak func c_name(a T) R     // and it may be missing at run time
extern blocking "z" func c_name(a T) R  // or a call may take a long time

struct Name { field T ... }
struct Name[T] { field T ... }     // generic; a type only once instantiated
extern struct Name { ... }         // declared field order, for C
interface Name { Method(a T) R ... }
interface Name { mut Method(a T) R }  // the method writes through its receiver
enum Name T { A; B = 3; C }        // T is the width, default int
error Name = "what went wrong"     // declared, with the sentence it says
error Name                         // declared, saying nothing
error A, B, C
type Name = T                      // another name for T, the same type

package name                       // must agree with the import path
import "path/to/package"
```

`mut` on a parameter or receiver means the callee may write through it. Only a
mutable binding can be passed to one, and `&x` only when `x` is one.

An interface method has no receiver to mark, so `mut` goes ahead of its name:
`mut Bump()` writes through whatever the interface value points at, and only a
mutable interface value can call it. A call through an interface is checked
against the interface's signature, since that is all the caller sees, so a
method satisfies it only if it writes nothing the interface does not allow: a
`mut` receiver needs `mut` on the interface method, and a `mut` parameter needs
`mut` on the same parameter there. A method that writes less is fine.

A copy of a slice or a pointer reaches the same memory as the original, so a
`mut` binding made from one lent read-only can be reassigned, and its own fields
written, but nothing it reaches through that copy:

```sword
func find(head ?*Node, key i32) ?*Node {
    mut cur := head        // fine: cur moves along the list
    cur = n.next           // fine
    cur.key = 0            // refused: cur was copied from head
}
```

A struct holding a read-only slice is the same: `p.at += 1` is the struct's own
field, `p.input[0] = 1` goes through the copy. Whether a method or a function
writes through what it is given is worked out from its body, so `p.Step()` is
allowed when Step only moves `p.at`. A call through an interface or a function
value is taken to write whatever its signature allows. Nothing can write through
an interface with no `mut` method, so a copy of one lends nothing read-only. A value with a read-only part cannot be
stored behind a pointer, and what a function hands back of its arguments keeps
the right it was given. The check does not follow order within a function: a
binding that is ever given read-only memory cannot be written through anywhere.

`type Name = T` gives a type a second name, nothing more: a `Name` is a `T`, and
values pass between the two without a conversion. It is for a name that says
what a type is for (`type Bytes = []u8`) or one that saves repeating a long one
(`type Set = collections.Map[string, bool]`), and an alias of a struct can be
used to write a literal or declare a method of it. An alias takes no type
parameters of its own, and is declared at the top of a file. `type` is a word
only there; a variable or field called `type` is a name like any other. A type
that is new rather than renamed is a struct or an enum.

A name is exported from its package when it starts with a capital letter. That
covers a struct's fields as well: a type can be public and its insides private,
which is why `time.Duration` has methods rather than a reachable `ns`.

### `det`

`det` on a function is a claim the compiler checks: what it answers depends on
its arguments and on nothing else — not on how many threads ran it, not on the
order they ran in, not on the clock, and not on where anything landed in
memory. It changes no generated code.

What takes it away, and each of these is a way for something other than the
arguments to reach the answer:

| | |
|---|---|
| an `atomic`, a `shared` | which task got there first decides |
| a channel shared by two senders or two receivers at once | the same |
| `select`, `TrySend`, `TryRecv`, `Len`, `Cap`, `Closed` | the timing decides |
| a channel copied, stored or returned | the compiler can no longer follow it |
| an `extern` call, or one through an `extern func` | the clock, the operating system, malloc's addresses |
| an address turned into a number | the layout decides |
| a call through an interface or a function value | unless every target is det |
| a call to anything that is not det | |

What is *not* on that list is the point: `scope`, `spawn` and `parallel for`
are all fine, and so is a channel with one sender and one receiver in each
scope that only ever waits for the next value — Kahn's condition, under which a
network of tasks computes a function of its inputs. Two tasks cannot touch the same memory — the race checker says
so — and a reduction combines its pieces in the order they were cut, so the
answer does not depend on the pool.

Every function is analysed, whether or not it says `det`; writing it asks to be
told when the property is lost, and the message walks the chain from what was
claimed to the call that lost it. A pointer is opaque to whoever is handed one,
so an arena doing arithmetic on addresses to align what it hands out does not
make its callers depend on the layout — only reading an address as a number
here does.

## Statements

```sword
if cond { } else if cond { } else { }   // `else` may also start the next line
if x := optional { } else { }
if mut x := optional { }          // a copy that can be written

for { }                            // until break or return
for cond { }
for i in a..b { }
for x in xs { }                    // elements of a slice, array or string
for i, x in xs { }                 // with the position
for part in xs.chunks(n) { }
for off, part in xs.chunks(n) { }
for x := optional { }              // while it holds something; `for mut x` too

switch subject { case a, b: ... default: ... }

parallel for i in a..b { }         // no break; continue skips one index
parallel for i in a..b reduce(+: acc) { }

scope { spawn f(args) ... }
lock name := shared_value { }
lock read name := shared_value { }  // readers together, a writer alone
if lock name := shared_value { } else { }   // only if it is free

break
continue
return expr
defer stmt
errdefer stmt
```

## Operators

By precedence, tightest first:

| | |
|---|---|
| `*` `/` `%` `<<` `>>` `&` | |
| `+` `-` `\|` `^` | `+` on strings only between constants |
| `==` `!=` `<` `<=` `>` `>=` | `==` on strings compares content |
| `&&` | short-circuits |
| `\|\|` | short-circuits |
| `catch` `orelse` | below everything else |

A NaN compares false against everything, itself included, so `x != x` is true
for one and is how it is recognised.

Prefix: `-` `!` `~` (every bit of an integer flipped) `&` (address of) `*`
(dereference) `try`. `x & ~m` clears the bits of `m`; xor is `^`, and `~` is
never a binary operator.

A statement standing alone has to do something — call, send or receive. One
whose value would only be thrown away, `x + 1` or `x == 2`, is an error.

Assignment: `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=` and the
wrapping forms below.

`+%` `-%` `*%` and `+%=` `-%=` `*%=` wrap on overflow and are never checked, in
any build mode.

`x << n` and `x >> n` with `n` at least the width of `x`'s type, or negative,
panic in a mode with checks. In `fast` and `small` the count is taken modulo the
width, so `x << 70` on a `u64` is `x << 6`: what the processor does anyway, at no
cost. A constant shifted outside `0..63` does not compile.

Dividing by zero, with `/` or `%`, panics in every mode, and so does dividing
the most negative value of a signed type by `-1`, whose answer does not fit. No
other result would be right, and next to a division the check costs little.

`[*]T` supports `p + n` and `p - n`. `*T` and `[]T` do not.

## Function values

A function named without being called is its address. There are no closures, so
a function value is one top-level function, one word wide, capturing nothing.

```sword
func apply(op func(i64, i64) i64, a i64, b i64) i64 {
    return op(a, b)
}

f := add                    // the function itself
mut maybe ?func() = nil     // one word, null meaning absent
```

Write `mut` on a parameter the function may write through: `func(mut []i64)` and
`func([]i64)` are different types. A generic function has no value — there is no
single function to point at. When a callback needs state alongside it, use an
interface: the state travels in the pointer.

### C functions as values

An `extern func` is a function C wrote. It has a type of its own, because a
`func` value can only point at a Sword function — that is what lets `det` follow
a call through one — and nothing could say which Sword function an address C
handed over really is.

```sword
extern func abs(x i32) i32
extern func qsort(base [*]u8, n u64, size u64,
                  less extern func([*]u8, [*]u8) i32)

f := abs                              // extern func(i32) i32
g := extern func(i32) i32(address)    // from a [*]T, typed by whoever writes it
p := [*]u8(f)                         // and back

func ascending(a [*]u8, b [*]u8) i32 { ... }
qsort(xs.ptr, n, 8, ascending)        // a Sword function, handed to C
```

A Sword function converts to an `extern func` of the same signature on its own:
with only scalars in it, Sword calls the way C does. It never converts back, and
an address never becomes a `func`. The signature follows the rule of an
`extern` declaration — scalars only, so a string is passed as `.ptr` and `.len`,
or through `strings.ToC` to a function that wants a zero at its end — and
cannot end in `...`, for the reason a variadic C function is only called
by name.

The conversion from an address is a promise nobody checks: if the function
there takes something else, the call is wrong in whatever way C would be. A C
function that calls back into Sword should do it on the thread that called into
C, while that call is still running.

## Variadic functions

The last parameter may gather the rest of the arguments, and inside the
function it is an ordinary slice:

```sword
func total(xs ...i64) i64
total(1, 2, 3)
total()                 // an empty slice
total(already...)       // pass a gathered list straight through
```

`...any` gathers a mixed list. An `any` carries the value together with a tag
saying what it is, filled in by the compiler from the static type at the call
site — there is no reflection anywhere. It is four fields:

| | |
|---|---|
| `Kind` | `u8`: 0 none, 1 bool, 2 int, 3 uint, 4 float, 5 string, 6 pointer |
| `Int` | signed, unsigned and bool values, widened to 64 bits |
| `Real` | floats, widened to `f64` |
| `Text` | strings |

`std/fmt` mirrors the tags as `KindBool`, `KindInt` and so on.

### C's variadic

An `extern` declaration may end in `...`, which is a different thing: it
gathers nothing, and says that the arguments after the declared ones travel by
the platform's rules.

```sword
extern func ioctl(fd i32, request u64, ...) i32
```

It matters. On Apple's arm64 a variadic argument goes on the stack while a
fixed one goes in a register, so declaring the arity you happen to use —
`ioctl(fd i32, request u64, p [*]u8)` — puts the third argument somewhere the
callee never looks. The same declaration passes on x86-64, which is the worst
way for it to be wrong.

What C promotes, Sword promotes: anything narrower than an `i32` arrives as
one, an `f32` as an `f64`. Only scalars fit through — a `string` is two words
here and a pointer there, so pass `.ptr` and `.len`. And a variadic function
can only be called by name: a function value carries one word and no
signature, which is not enough to know where the extra arguments go.

## Packages and modules

A package is a directory, and every `.sword` file in it shares one scope. It is
imported by its path and called by the last element of that path:
`import "std/mem"` is `mem`. A `package` clause, when a file has one, has to say
the same name. A path whose last element is not a name a program can write —
`github.com/ana/sword-sqlite` — takes its name from the clause instead, and a
path ending in a major version — `.../sqlite/v2` — from the element before it.
Two imports that would give the same name are an error, and one of them is
given another name where it is imported:

```sword
import "example.com/deep"
import deep2 "example.com/deep/v2"
```

That name holds for the whole package, every file of it, and the package's own
name no longer reaches it there.

A comment directly above a declaration is its documentation, and one above a
`package` clause documents the package. `shield doc` prints them beside what
they describe, as it was written:

```
shield doc std/http                # everything it exports, a line each
shield doc std/http.Server         # one type, its fields and its methods
shield doc std/http.Server.Close   # one method
shield doc mem.Arena               # a package of std needs no std/
shield doc .                       # the package here
```

Fields and methods the package keeps to itself are left out. Inside a module,
`shield doc` finds the module's packages and its dependencies the way a build
would, though it never fetches one.

A module is a tree of packages with a `sword.mod` at its root. The compiler finds
it by looking in the directory being built and every directory above:

```
module example.com/site
sword 0.3

require github.com/ana/sqlite v1.4.0
replace example.com/lib => ../lib
```

`shield new example.com/site` starts one: a `site` directory with the
`sword.mod`, a `main.sword` and a test for it, in a git repository of its own
unless it is made inside one. `shield new --lib example.com/geo` starts a
library instead, a package with one function and its test; when the last element
of the path is not a name a program can write, the package's name follows it:
`shield new --lib example.com/sword-geo geo`.

`module` names the tree, and its packages are imported under that name:
`example.com/site/util` is the `util` directory beside the `sword.mod`. A module
path is lower-case letters, digits, `.`, `-` and `_`, in segments joined by `/`,
and nothing under `std`. `sword` is the oldest compiler the module builds with,
and a newer requirement is refused rather than half compiled. `replace` points a
path, and everything under it, at a directory, relative to the module's root.
Lines starting with `//` are comments.

### Dependencies

`require` names another module and the version wanted of it. A module lives at
the root of a git repository, and its path is where that repository is:
`github.com/ana/sqlite` is fetched from `https://github.com/ana/sqlite.git`,
with the credentials git already has. A version is a tag, `v1.4.0`. Each major
version from 2 on is a module of its own, imported under its own path —
`github.com/ana/sqlite/v2`, with `module github.com/ana/sqlite/v2` in its
`sword.mod` and tags `v2.x.y` — so a program can use both while it moves from one
to the other.

```
shield get github.com/ana/sqlite           # the newest release
shield get github.com/ana/sqlite@v1.4.0    # this one
shield get github.com/ana/sqlite@main      # a branch or a commit
shield get github.com/ana/sqlite@none      # take it out
```

`shield get` writes the `require` line and fetches the module. A commit with no
tag gets a version of its own, `v0.0.0-20261023-a1b2c3d4e5f6`, the day it was
made and its hash, which sorts before every release: it is for using a fix before
its author has released it. Without a version, `get` takes the newest release,
and never goes back from a newer version already required.

A build fetches whatever `sword.mod` requires and is not on this machine yet, and
nothing else: importing a module nobody required is an error, and so is
importing one the program only has because a dependency required it. A
dependency, in turn, imports only what its own `sword.mod` requires. Fetching a
module never runs any of its code; a Sword package has no build script.

When several modules require the same one, the build uses the newest version
any of them asks for — not the newest that exists. A release nobody asked for
never enters a build, so the same `sword.mod` builds the same code next year.
Only the module being built may `replace`; a dependency's `replace` lines are
ignored.

`sword.sum`, beside `sword.mod`, holds a hash of every module version the build
has read, and both belong in version control:

```
github.com/ana/sqlite v1.4.0 sha256:3f1c...
```

The hash covers the files the compiler reads, `.sword` files and `sword.mod`.
A version whose code no longer matches its line — a tag moved to other code — is
refused. A build adds the line for a version it has not seen before;
`--locked` refuses to, for a build that has to use exactly what was committed.

Fetched modules are kept in `~/.sword`, or wherever `SWORD_CACHE` says, one
directory per version, read-only, with the code and its licence and nothing
else. `shield vendor` copies every dependency into `vendor/` with a
`vendor/modules.txt` listing them, and from then on a build reads `vendor/`
instead, with no network and no cache. It is checked against `sword.sum` on
every build, since it sits where anybody can edit it, and after `sword.mod`
changes, `shield vendor` is run again.

## C libraries

A string after `extern` names the library a function comes from, the way `-l`
would: `"z"` is libz, `"sqlite3"` is libsqlite3. That is all it takes to link
it — nothing on the command line, and nothing for whoever builds a program that
imports the package doing the declaring.

```sword
extern "sqlite3" func sqlite3_libversion_number() i32
extern "z" func zlibVersion() [*]u8
```

It is a name and never a path or an option; those belong to `--link`, where
whoever builds the program can see them. Two packages may declare the same
function, and must then agree on where it comes from.

How a library is linked is a question for the build rather than the source,
since one package is built both ways depending on where the program is going.
By default it is linked dynamically. `shield --static` takes every library the
program declared from its archive, `lib<name>.a`, and refuses to build when
there is none rather than quietly linking the other way; whatever those
archives need comes from an archive too when there is one. The system's own
libraries — libc, libm, pthreads, the C++ library — always stay dynamic: an
archive of libm beside a shared libc is two copies of one library in one
process. On Linux that means a `--static` program still needs the C library of
the machine it runs on, and still resolves names and loads libraries the
ordinary way.

Where a library lives is asked of pkg-config when the machine has it — under the
name in the source, then with `lib` in front, which is how `extern "curl"` finds
libcurl's file. What it says is used as it says it, so a library whose file
names something else is still found. Without pkg-config, or for a library it
does not know, the name goes to the linker as `-l<name>`, searched in the usual
places and in every directory given with `--link -L<dir>`. A shared library
found in a directory like that is also looked for there when the program starts.

### Weak functions

`extern weak` declares a function the program can run without — one that only
newer versions of a library have, say. The program links and starts whether or
not anything provides it, and its name is an `?extern func`, nil when nothing
did. Calling it by name is an error, because a missing one would be a jump to
address zero:

```sword
extern weak func getentropy(buf [*]u8, n u64) i32

if fill := getentropy {
    try check(fill(room.ptr, room.len))
} else {
    // the older way
}
```

`weak` is about the function, not the library: a library named after it has to
be there when the program starts. A library that may be missing altogether is
loaded with [`std/dl`](#stddl).

### Blocking functions

A task waiting on a socket, a lock or a channel is put down and its thread
moves on to other tasks. A C function cannot be put down: while it waits, the
thread waits with it, and a few such calls at once hold every thread the
scheduler has. `blocking` says a function may take its time — a database query,
a read from a device, a call into a library that does its own network I/O:

```sword
extern blocking "sqlite3" func sqlite3_step(stmt c.Ptr) c.Int
```

Each call to it is made on the threads kept for files and name resolution, and
the task is put down until it returns, exactly as a file read is. Outside any
task the call is made where it is. `errno` comes back with the answer, so
`os.Errno()` right after the call is the one the function left.

A blocking function is called by name only. As a value it would be an ordinary
`extern func`, and a call through it would hold a thread with nothing in the
code to say so. That is why `blocking` takes the place of `weak`, which is only
called through its value, and why a blocking function cannot take C's `...`.
The hand-off costs a few microseconds, which is nothing next to a call that
blocks and too much for one that does not: leave `blocking` off a function that
only computes.

## Atomics

`atomic[T]` over an integer or a bool. It is the one thing several tasks may
write at the same time; the race checker lets it through and everything else
through only for reading.

```sword
mut hits := atomic[u64](0)

hits.Load()                  // T
hits.Store(v)
hits.Swap(v)                 // T, the previous value
hits.Add(v)                  // T, integers only
hits.Sub(v)
hits.And(v)
hits.Or(v)
hits.CompareSwap(old, new)   // bool: whether it matched
```

Every one of these is a single machine instruction, with sequentially
consistent ordering.

## Shared values

`shared[T]` over anything. Where an atomic covers one scalar, this covers a
whole structure — a map, a queue, a cache — by putting a mutex in front of it.

```sword
mut table := shared[collections.Map[string, u64]](
    try collections.NewMap[u64](&a, 64))

lock m := &table {
    seen := m.Get(key) orelse 0
    try m.Set(key, seen + 1)
}
```

`shared[T](v)` is the only way to build one and `lock` is the only way to reach
the value, which is what makes the race checker's exemption safe. Inside the
block the name is a mutable `*T`; the brace releases the lock, and so does a
`return`, a `break`, a `continue` or a `try` that fails.

`lock read` lets any number of readers in at once, and no writer while any of
them is there. Inside it the name is an immutable `*T`, so nothing reached
through it can be written:

```sword
lock read m := &table {
    seen := m.Get(key) orelse 0
}
```

A writer waiting keeps new readers out, so a steady stream of them cannot keep
it waiting for good.

`if lock` takes the lock only when that would not mean waiting, and runs the
`else` branch, without it, when somebody has it. It works for both kinds:

```sword
if lock m := &table {
    try m.Set(key, 1)
} else {
    // busy: come back later
}
```

Three operations beside `lock`, all called with the lock held, and held to
write — readers hold it together, and only one holder may wait or wake:

```sword
func (s *shared[T]) Wait()        // let go, wait to be told, take it back
func (s *shared[T]) Notify()      // wake one waiter
func (s *shared[T]) NotifyAll()   // wake all of them
```

`Wait` is what turns a mutex into something a queue can be built out of — it is
what a channel is written on. A caller loops rather than testing once, because
what it waited for may be gone again by the time it wakes.

No `mut` is needed anywhere, the same way an atomic needs none: the type says it
will be written by whoever holds the lock, so the binding's mutability has
nothing left to say. A shared may also sit in a struct field, and the struct
around it stays ordinary.

The rules around it:

- a `lock` inside another on the same value is an error, and the runtime aborts
  with a message when the second one comes through a call;
- `spawn` under a lock whose `scope` is outside it is an error: the task would
  still be running when the block released. Put the `scope` inside the `lock`,
  or the `lock` inside the task.
- a reader holds no name the runtime can check, so a task that reads a value and
  then, through a call, asks to write it waits for itself; the checker refuses
  the case it can see.

A contended lock spins briefly and then puts the task down until it is its
turn, so a thread stuck behind a long critical section goes on to other work.

## Switch

```sword
switch subject {
case a, b:
    ...
case c:
    ...
default:
    ...
}
```

The subject is an integer, a bool, a string or an enum. No fallthrough: a case
body ends where the next `case` begins. Two cases matching the same constant is
an error, and so is a second `default`.

A `switch` is not a loop, so `break` and `continue` inside one belong to the loop
around it.

Over an enum with no `default`, every member has to be covered:

```
error: this switch over Kind does not cover Bool, Int, Text
note: add the missing cases, or a 'default'
```

## Enums

```sword
enum Kind u8 {
    None            // 0
    Bool            // 1
    Text = 9
    Real            // 10
}
```

The width is optional and defaults to `int`. A member with no value continues
from the one before, starting at zero; two members with the same value is an
error.

A member is named through its type — `Kind.Text`, or `json.Kind.Object` from
another package. The type is its own, so nothing arithmetic reaches it by
accident:

| | |
|---|---|
| `==` `!=` | yes |
| `switch` | yes, and checked for completeness without a `default` |
| `+` `<` and the rest | no |
| `u8(k)`, `Kind(n)` | yes, and **not** checked against the members |

`nameof(k)` is the name of the member `k` is, and the empty string when it is not
one. It works on an error too. Printing an enum gives the name for the same reason, so the number takes a
conversion:

```sword
try io.Printf("{} is {}\n", k, u8(k))    // Real is 10
```

## Reductions

```sword
parallel for i in a..b reduce(op: acc) { ... }
```

`op` is `+`, `&`, `|`, `min` or `max`, or the name of a function that combines
two partial answers into one:

```sword
mut total := num.NewKahan()
parallel for i in 0..xs.len reduce(num.Merge: total) {
    total.Add(xs[i])
}
```

That function takes two of whatever is being reduced and answers a third —
`func(T, T) T`, writing through nothing — and each piece of the range starts
from the zero value of `T`, so this is for accumulators where all-zero means
"nothing yet". `&` and `min` stay built in because their identities are not
zero.

Only `+` applies to a float among the built-in operators. The body
combines one element into the private copy that piece of the range has —
`acc += xs[i]` for a sum, `if xs[i] > acc { acc = xs[i] }` for a maximum — and
the clause says how those copies are combined at the end.

The answer does not depend on how many threads ran it. The range is cut into a
fixed number of pieces, each keeps its own partial answer, and they are
combined in the order they were cut. That matters for floating point, where
adding in a different order is a different number: the same program answers the
same bits on one thread and on sixteen. How many tasks run those pieces is
still up to the scheduler — it is the cut that is fixed, not the schedule.

A float `+` fixes the order inside each piece as well, as one a vector unit can
follow. The piece keeps sixteen sums, all starting at zero, and its k-th
element, counting from the start of the piece, is added into sum `k % 16`;
within the body, the accumulator is the sum the current element goes into. At
the end the sums are folded by halves — sum `j` adds sum `j + 8` for each `j`
below 8, then `j + 4`, `j + 2` and `j + 1` — and sum 0 is the piece's answer.
Sixteen is the language's number, not the machine's: a wider or a narrower
vector unit gives the same bits.

It is not the same number a sequential loop would reach, since the pieces are
summed separately before being combined. Reproducible, not sequential.

## Errors

```sword
error Name = "what went wrong"  // declared at the top level, message optional
error A, B, C

func f() !T                    // may fail
return error.Name              // fail with a code
return value                   // succeed
try expr                       // propagate a failure to the caller
expr catch fallback            // substitute a value
expr catch |e| { ... }         // handle it, naming the error
expr catch { ... }             // handle it without naming it
```

`nameof(e)` gives an error's name and `e.Message()` the sentence it was declared
with — the compiler writes both tables, because at run time an error is a code and
nothing else. A code that is not an error, zero among them, answers the empty
string for both. An error declared without a message has an empty one.

Errors are **declared**, and using one that is not is a compile error. That is the
point of the declaration: `error.Tiemout` used to compile and become a second
error nobody handled.

Codes are global to the program, so the same name raised in two packages is one
error and a caller handles it once — but the two declarations have to agree on the
message, and the compiler says so if they do not. The standard library's errors
have the same codes in every program, whether it imports their package or not;
a program's own come after them. A lowercase name is private to
the package that declared it, like everything else. A `!T` cannot be discarded, and
its fields cannot be reached until it is handled.

The message is a string literal, so it costs no allocation: it sits in the binary
and an error that goes unhandled costs nothing to carry. That is why there is no
`errors.New` taking a formatted string — building a message is the caller's job,
at the point where it has somewhere to put it.

A handler block that has to produce a value must leave the scope. In statement
position nothing is expected of it.

`func main() !int` and `func main() !void` both work; an error that reaches
`main` becomes the exit status. An exit status is eight bits, so a code past 255
leaves as 255 rather than wrapping round to 0, which would say it worked.

## Optionals

```sword
var ?*T = nil
if x := optional { ... } else { ... }
optional orelse fallback
optional orelse return error.Name
optional == nil                // present or not
optional != nil
```

Comparing against `nil` is the only comparison an optional has: two of them
would mean comparing payloads, and an absent one has none.

## Conversions

`T(x)` between numeric types, between raw pointers, and between `string` and
`[]u8` in either direction. `*T` converts to `[*]T`, never the other way — that
would fabricate a non-null guarantee. A raw pointer and an `extern func` convert
both ways; a `func` converts to an `extern func` of the same signature and to
nothing else.

`p[a..b]` on a `[*]T`, with both bounds written, produces a `[]T`. It is the
crossing from unchecked memory to checked.

## Members

| | |
|---|---|
| `xs.len` | slices, arrays, strings |
| `xs.ptr` | slices and strings, as `[*]T` |
| `s.field` | struct fields, auto-dereferencing through `*T` |
| `x.Method()` | methods, and interface dispatch |

## Generics

```sword
func F[T](x T) T
func F[T: Constraint](x *T) R
func (v *Box[T]) Method() T        // methods on a generic struct
F(value)               // type argument inferred
F[i32](value)          // written out
sizeof[T]()            // compile-time constant
alignof[T]()
hashof(x)              // u64, for a value `==` can compare
```

Constraints are interfaces. A call through a constraint is resolved when the
function is instantiated and compiles to a direct call; a call through an
interface parameter goes via the method table.

`hashof` takes an integer, an enum, a bool or a string — what `==` compares,
since a hash is only worth anything beside an equality that agrees with it. It
is what lets a generic table key itself on whatever it was given: a string goes
through FNV-1a, anything else through a few instructions of mixing. Anything
else is refused where the generic is instantiated, which is where the caller
can see what it asked for.

Type arguments written out have to be type names. Composite types come from
inference.

## Standard library

### `std/io`

Printing takes a fixed block of stack and flushes as it fills, so none of it
needs an allocator.

```sword
func Print(s string) !void                              // stdout
func Fail(s string) !void                               // stderr
func Write(fd i32, s string) !u64                       // bytes written

func Printf(format string, args ...any) !void
func Errorf(format string, args ...any) !void
func Fprintf(fd i32, format string, args ...any) !void
func Println(args ...any) !void                         // spaced, newline

// A Writer is a buffered sink over a descriptor; it satisfies fmt.Sink.
func NewWriter(fd i32) Writer
func (mut w *Writer) WriteByte(c u8) !void
func (mut w *Writer) Write(p []u8) !void
func (mut w *Writer) Flush() !void
```

### `std/mem`

```sword
interface Allocator {
    mut Alloc(n u64, align u64) ?[*]u8
    mut Release(mut p [*]u8, n u64)
}

func Align(offset u64, align u64) u64

func Alloc[T](mut a Allocator, n u64) ?[]T
func Free[T](mut a Allocator, xs []T)
```

`Arena` — bumps through a buffer it does not own:

```sword
func NewArena(backing []u8) Arena
func (mut a *Arena) Alloc(n u64, align u64) ?[*]u8
func (mut a *Arena) Release(mut p [*]u8, n u64)     // does nothing
func (mut a *Arena) Reset()
func (a *Arena) Used() u64
```

`Arena.Watch` is a field: set it and `Reset` writes `mem.Poison` (0xDE) over
everything it takes back, so anything still holding memory from before the reset
reads something obviously wrong. `shield test` sets it on every test's arena.
`Watched` does the same for memory released one piece at a time:

```sword
func NewWatched(a Allocator) Watched            // { inner, Written }
func (mut w *Watched) Alloc(n u64, align u64) ?[*]u8
func (mut w *Watched) Release(mut p [*]u8, n u64)   // writes over it first
```

Both cost a write per byte released, so neither is on by default. See [the tour on
memory that has gone away](tour.md#memory-that-has-gone-away).

`System` — over C `malloc` and `free`:

```sword
func NewSystem() System
func (mut s *System) Alloc(n u64, align u64) ?[*]u8
func (mut s *System) Release(mut p [*]u8, n u64)
func (s *System) Live() u64
```

An allocator is not safe to share between tasks, and the race checker says so:
handing the same one to two `spawn`s is a compile error. Give each task its own.

### `std/bytes`

A buffer that grows, holding the allocator it was built with:

```sword
func New(mut a mem.Allocator, capacity u64) !Buffer
func (b *Buffer) Len() u64
func (b *Buffer) Bytes() []u8
func (b *Buffer) Str() string
func (mut b *Buffer) Reset()
func (mut b *Buffer) Free()
func (mut b *Buffer) WriteByte(c u8) !void
func (mut b *Buffer) Write(from []u8) !void
func (mut b *Buffer) WriteString(s string) !void
func (mut b *Buffer) WriteU64(v u64) !void
func (mut b *Buffer) Insert(at u64, from []u8) !void
func (mut b *Buffer) Delete(at u64, count u64) !void
func (mut b *Buffer) Truncate(to u64)
```

`Insert` grows the way `Write` does and moves the tail up; `Delete` moves it
back down. A range that is not inside the buffer is `error.OutOfRange` rather
than a clamp — `at` may be the length, which appends, and nothing past it.
`Truncate` only ever shortens.

### `std/strings`

Byte-oriented, which is what a protocol parser wants. `IndexByte` and `Index`
return the length of the haystack when there is no match. `Concat` and `Join`
are the only two that allocate, so they are the only two that take an
allocator; the answer is measured first and allocated once, at its exact
length.

```sword
func Equal(a string, b string) bool
func EqualFold(a string, b string) bool      // case-insensitive
func HasPrefix(s string, prefix string) bool
func HasSuffix(s string, suffix string) bool
func IndexByte(s string, c u8) u64
func LastIndexByte(s string, c u8) u64
func Index(s string, needle string) u64
func Contains(s string, needle string) bool
func TrimSpace(s string) string
func ToLower(c u8) u8
func ParseU64(s string) !u64
func ParseF64(s string) !f64                 // the nearest double, exactly

func Concat(mut a mem.Allocator, parts ...string) !string
func Join(mut a mem.Allocator, parts []string, sep string) !string

func FromC(p [*]u8) string                   // up to the zero, not copied
func FromCMax(p [*]u8, most u64) string      // and at most `most` bytes
func ToC(s string, mut into []u8) ![*]u8     // s and a zero, into `into`
```

A C string is a pointer to bytes ending in a zero; a Sword string is a pointer
and a length, with no zero after it. `FromC` reads up to the zero and answers
with a view of that memory, so a string from a buffer the library reuses or
frees has to be copied out before it is kept. `FromCMax` also stops after
`most` bytes, for a `char[N]` field that the text fills, which then has no zero
at all. `ToC` writes the string and its zero into a buffer of the caller's —
`error.NoSpace` when it does not fit, and `error.InteriorNul` when the string
holds a zero itself, since C would read a different string from the one
passed.

```sword
extern func getenv(name [*]u8) ?[*]u8

mut room := [64]u8{}
p := getenv(try strings.ToC("HOME", room[..])) orelse return ""
home := strings.FromC(p)
```

`ParseF64` takes an optional sign, digits with an optional point, and an
optional exponent — `-12.5e3`, `.5`, `7.` — and answers the double nearest the
exact value written, a tie going to the even one. So text printed with enough
digits reads back as the same double, and the same text gives the same bits on
every machine. `std/json` reads its numbers with it.

### `std/unicode`

Where a character starts, what it cost, and how wide it is. Nothing here
allocates.

```sword
struct Rune { Code u32; Bytes u64 }

func Decode(s string, at u64) ?Rune       // nil off a boundary or off UTF-8
func Encode(r u32, mut into []u8) !u64    // bytes written
func Valid(s string) bool
func Count(s string) u64                  // characters, not bytes
func Width(r u32) u64                     // 0, 1 or 2 columns
```

`Decode` answers nil for three different things — past the end, inside a
character, and bytes that are not UTF-8 — because none of them should come
back as a character nobody wrote. `Bytes` is what a cursor moving right adds
to its position; stepping by one byte instead lands in the middle of a
character, and the terminal draws something that is not what is in the file.

`Width` is what decides whether a screen lines up: a combining mark hangs off
the character before it and takes no column, and a CJK character takes two.
The ranges it knows are the ones that decide a terminal — the combining
blocks, East Asian Wide and Fullwidth, and the emoji every terminal draws
double — rather than the whole of Unicode, which is a standard and a generated
table. Normalisation, case folding past ASCII and grapheme clusters are not
here.

### `std/collections`

```sword
func NewList[T](mut a mem.Allocator, capacity u64) !List[T]
func (l *List[T]) Len() u64
func (l *List[T]) Cap() u64
func (l *List[T]) Slice() []T          // invalid after the next Push
func (l *List[T]) At(i u64) T
func (mut l *List[T]) Set(i u64, v T)
func (mut l *List[T]) Push(v T) !void
func (mut l *List[T]) Pop() ?T
func (mut l *List[T]) Reset()
func (mut l *List[T]) Free()
```

A hash map, open addressing and linear probing:

```sword
func NewMap[V](mut a mem.Allocator, capacity u64) !Map[string, V]
func NewMapOf[K, V](mut a mem.Allocator, capacity u64) !Map[K, V]
func Hash(key string) u64
func (m *Map[K, V]) Len() u64
func (m *Map[K, V]) Get(key K) ?V
func (m *Map[K, V]) Has(key K) bool
func (mut m *Map[K, V]) Set(key K, value V) !void
func (mut m *Map[K, V]) Delete(key K) bool
func (mut m *Map[K, V]) Free()

// Iteration is by slot: walk 0..Slots() and ask each one.
func (m *Map[K, V]) Slots() u64
func (m *Map[K, V]) KeyAt(i u64) ?K
func (m *Map[K, V]) ValueAt(i u64) V
```

The key is anything `hashof` and `==` both understand — an integer, an enum, a
bool or a string — so a table from a key code to a command needs no string to
look itself up with. `NewMap` is the string case, which most of them are; a key
of another kind is refused where the map is instantiated.

Sorting, over a comparison that is an ordinary function value:

```sword
func Sort[T](mut xs []T, less func(T, T) bool)
func BinarySearch[T](xs []T, target T, less func(T, T) bool) ?u64
```

`Sort` is an introsort — quicksort, insertion sort for short runs, heapsort
once the recursion goes deeper than it should — so an adversarial input costs
n log n rather than n². It allocates nothing and it is not stable.
`BinarySearch` answers the leftmost index that compares equal, and nil when
there is none; a slice that is not sorted by the same `less` gives a wrong
answer rather than a slow one.

### `std/fmt`

Everything writes into a sink rather than one particular buffer, so the same
code serves a growable buffer, a socket, or a block of stack.
`bytes.Buffer` and `io.Writer` both satisfy it without being told to.

```sword
interface Sink {
    mut WriteByte(c u8) !void
    mut Write(p []u8) !void
}

func Format(mut out Sink, format string, args []any) !void
func Sprintf(mut a mem.Allocator, format string, args ...any) !string
func Value(mut out Sink, v any) !void

func Str(mut out Sink, s string) !void
func U64(mut out Sink, v u64) !void
func I64(mut out Sink, v i64) !void
func Bool(mut out Sink, v bool) !void
func F64(mut out Sink, v f64, decimals u64) !void
func Float(mut out Sink, v f64) !void     // the shortest text that reads back
func Pad(mut out Sink, s string, width u64) !void
func Quote(mut out Sink, s string) !void  // JSON string, escaped

struct Base {
    Mask u64
    Inc u64
}
func BaseConverter(mut out Sink, v u64, base Base, width u64) !void 
```
The digits are exact: every double is m·2^e, its decimal expansion ends, and
`F64` prints that expansion to the places asked for, rounded once with a tie
going to the even digit — `{.20}` of `0.1` is `0.10000000000000000555`, as in C.
Two different doubles never print the same when enough places are asked for.
`F64` is fixed point at any size, so `1e300` prints all 301 digits.

`Float`, which is what `{}` uses, prints the shortest text that reads back as the
same double: `0.1` is `0.1`, `1.0 / 3.0` is `0.3333333333333333`, and `0.1 + 0.2`
is `0.30000000000000004`, because that is the double it is. It is plain from
`1e-4` up to `1e16`, with `.0` on a whole number so that it still reads as a
float, and a mantissa with a power of ten outside that: `1e+16`, `1.5e-7`.
Negative zero prints with its sign. `nan`, `inf` and `-inf` are named.

The format language is small on purpose:

| | |
|---|---|
| `{}` | the next argument, formatted by what it is |
| `{b}` | an integer in binary |
| `{q}` | an integer in quaternary |
| `{o}` | an integer in octal |
| `{x}` | an integer in hexadecimal |
| `{.N}` | a float with N decimal places |
| `{{` `}}` | a literal brace |

### `std/json`

A parsed document is one flat list of nodes; children are reached by index
rather than by pointer, so the whole tree is a single allocation.

```sword
func Parse(input []u8, mut a mem.Allocator) !Document

func (d *Document) Root() u64
func (d *Document) Kind(at u64) Kind       // Null Bool Number Str Array Object
func (d *Document) Text(at u64) string
func (d *Document) Number(at u64) f64
func (d *Document) Truth(at u64) bool
func (d *Document) Len(at u64) u64         // array or object size
func (d *Document) At(at u64, i u64) ?u64
func (d *Document) Get(at u64, key string) ?u64
func (d *Document) KeyAt(at u64) string
func (d *Document) GetText(at u64, key string) string
func (d *Document) GetNumber(at u64, key string, fallback f64) f64
```

Writing is a streaming writer that puts the commas in for you. No write asks
for `try`: the first thing that goes wrong — memory running out, a `TooDeep` or
an `Unbalanced` — is kept, every write after it does nothing, and `Err` says
what it was once the document is done:

```sword
mut w := json.NewWriter(&buffer)
w.BeginObject()
w.Key("name")
w.Str("sword")
w.Key("tags")
w.BeginArray()
w.Str("fast")
w.EndArray()
w.EndObject()
try w.Err()            // the first thing that went wrong, if anything did
```

### `std/crypto`

Nothing here allocates. A hasher is a value and a digest is an array of bytes,
so hashing inside a request handler costs no arena and no allocator parameter.

```sword
const Sha256Size = 32

func Sha256Of(data []u8) [Sha256Size]u8    // the whole message at once

func NewSha256() Sha256
func (mut h *Sha256) Write(p []u8)         // any amount at a time
func (mut h *Sha256) Sum() [Sha256Size]u8
```

`Write` keeps whatever does not fill a block, so a file can be hashed as it is
read rather than after it is read:

```sword
mut h := crypto.NewSha256()
mut buf := [32768]u8{}
for {
    n := try file.Read(buf[0..32768])
    if n == 0 {
        break
    }
    h.Write(buf[0..n])
}
digest := h.Sum()
```

`Sum` finishes the message. The padding it writes is part of what was hashed,
so there is nothing sensible to append afterwards: a hasher is spent once its
digest has been taken, and another message needs another hasher.

HMAC-SHA-256 has the same shape, with the key given when the value is built:

```sword
func HmacSha256Of(key []u8, message []u8) [Sha256Size]u8

func NewHmacSha256(key []u8) HmacSha256
func (mut h *HmacSha256) Write(p []u8)
func (mut h *HmacSha256) Sum() [Sha256Size]u8

func Equal(a []u8, b []u8) bool            // in time that does not depend on the bytes
```

A tag that arrived from outside is checked with `crypto.Equal`, never with a
loop that stops at the first difference: how long that loop takes tells the
sender how much of a guess was right, and a tag can then be guessed a byte at a
time.

```sword
want := crypto.HmacSha256Of(secret, payload)
if !crypto.Equal(want[0..crypto.Sha256Size], tag) {
    return error.BadSignature
}
```

PBKDF2-HMAC-SHA-256 derives a key from a password, filling the slice it is
given whatever its length:

```sword
error BadRounds = "a derived key needs at least one round"

func Pbkdf2Sha256(password []u8, salt []u8, rounds u32, mut into []u8) !void
```

A round count of zero is refused rather than read as one. It often comes from
the other end of a connection, and a peer asking for a password hash one HMAC
deep is not asking for something to be quietly granted.

Keys, salts and tokens are made from the system's random source:

```sword
error NoEntropy = "the system would not give random bytes"

func Random(mut into []u8) !void
```

```sword
mut token := [32]u8{}
try crypto.Random(token[0..32])
```

`std/rand` is the wrong package for this. Its numbers are a pure function of
the seed, which is what a simulation wants and exactly what a secret must not
be.

### `std/encoding`

Bytes as text and back: hex, and base64 in the two alphabets of RFC 4648.
Like `std/crypto` it allocates nothing — each function writes into the slice it
is given and answers with the part it filled, and a slice too small is
`error.NoSpace`, never a write past its end.

```sword
func Hex(src []u8, mut into []u8) !string           // lowercase
func DecodeHex(text string, mut into []u8) !u64     // either case

func Base64(src []u8, mut into []u8) !string        // + and /, padded with =
func Base64Url(src []u8, mut into []u8) !string     // - and _, no padding
func DecodeBase64(text string, mut into []u8) !u64
func DecodeBase64Url(text string, mut into []u8) !u64

func HexSize(n u64) u64        // the room an encoding of n bytes takes
func Base64Size(n u64) u64
func Base64UrlSize(n u64) u64
```

The standard alphabet is what SCRAM, MIME and HTTP Basic authentication use;
the URL one is what goes into a URL, a cookie or a JWT, where `+`, `/` and `=`
would have to be escaped.

```sword
mut token := [32]u8{}
try crypto.Random(token[0..32])
mut room := [43]u8{}
cookie := try encoding.Base64Url(token[0..32], room[..])
```

Decoding is strict. Padding is required in the standard form and optional in
the URL one, nothing outside the alphabet is accepted — not a line break, not
a space — and the unused bits of the last character must be zero. A lenient
decoder reads several texts as the same bytes, and then a token that was
checked after decoding no longer has one spelling.

### `std/num`

Adding floating point loses the small terms: once the running total is large
enough, a term below its last bit changes nothing. A compensated sum keeps what
each addition threw away.

```sword
func NewKahan() Kahan
func (mut k *Kahan) Add(v f64)
func Merge(a Kahan, b Kahan) Kahan   // two partial sums into one
func (k Kahan) Value() f64           // the total, compensation included
func (k Kahan) Rounded() f64         // without it: what plain addition reaches
func Abs(v f64) f64
```

Neumaier's form, which is Kahan's plus the case Kahan's misses — a term larger
than the running total, where it is the total that gets rounded away. A million
ones added to 1e16 come to 1e16 by plain addition, and to 1e16 + 1000000 here.

`Merge` is what `parallel for ... reduce(num.Merge: total)` calls, and the zero
value is the identity, so it is also the shape any reduction of your own takes.

An exact sum goes further: it holds the total in a fixed-point number wide
enough for every double (Kulisch's long accumulator, 68 words), so no term is
rounded and `Value` rounds once, to the double nearest the true sum. The order
of the terms cannot show, and a parallel sum is the same double as a sequential
one.

```sword
func NewExact() Exact
func (mut s *Exact) Add(v f64)
func MergeExact(a Exact, b Exact) Exact  // for reduce(num.MergeExact: total)
func (s Exact) Value() f64               // correctly rounded
```

The elementary functions give the same bits on every machine: they use only
IEEE `+ - * /`, which every processor rounds alike, and the compiler never fuses
a multiply into an add. Each is within one unit in the last place of the true
value, and `Sqrt` is correctly rounded. They are det, which the C library's are
not — a det function cannot call out of the language.

```sword
func Sqrt(x f64) f64
func Exp(x f64) f64
func Log(x f64) f64
func Sin(x f64) f64       // any finite x: the reduction is exact
func Cos(x f64) f64
func Inf() f64
func NaN() f64
func IsNaN(v f64) bool
```

### `std/par`

Operations over a slice that use every core and answer the same thing at any
thread count. The cut depends on the length alone — at most 64 pieces, none
under 1024 elements — and the pieces are put together in the order they were
cut. Each is det when the function it is given is.

```sword
func Scan[T](xs []T, mut out []T, op func(T, T) T) !void   // out[i] = xs[0] op .. op xs[i]
func Keep[T](mut a mem.Allocator, xs []T, mut out []T,
             keep func(T) bool) !u64                       // in order; how many
func MinIndex[T](xs []T, less func(T, T) bool) ?u64        // the first, on a tie
func Sort[T](mut xs []T, less func(T, T) bool)             // stable, in place
```

`Scan` equals the sequential answer when `op` is associative; for floating-point
addition it is a fixed answer rather than the left-to-right one. `Keep` tests on
every core and gathers the survivors in one pass, and its allocator holds a
byte per element for the marks. `Sort` is stable, which is what makes it
deterministic with equal keys: there is exactly one right order. It merges in
place, the way Go's `sort.Stable` does, so it needs no memory of its own.

### `std/rand`

Random numbers where the i-th one depends on the seed and i and nothing else, so
a parallel loop draws the same numbers however many threads run it. Philox4x32-10,
the generator cuRAND and JAX use for that reason. Not for keys or tokens.

```sword
func At(seed u64, i u64) u64          // the i-th number; the first of Stream(seed, i)
func Stream(seed u64, i u64) Source   // as many as iteration i needs, its own
func (mut s *Source) U64() u64
func (mut s *Source) Below(n u64) u64 // uniform in [0, n), unbiased
func (mut s *Source) F64() f64        // uniform in [0, 1), 53 bits
```

Give each iteration its own stream, indexed by the iteration, and a det function
can use randomness:

```sword
parallel for i in 0..n reduce(+: inside) {
    mut r := rand.Stream(seed, u64(i))
    x := r.F64()
    y := r.F64()
    if x * x + y * y < 1.0 {
        inside += 1
    }
}
```

### `std/os`

```sword
func Argc() u64                   // including the program's own name
func Arg(i u64) string            // empty past the end
func Args(mut into []string) []string
func Env(name string) ?string     // nil when unset
func EnvOr(name string, fallback string) string
func Exit(code i32)               // no defers run, no output is flushed

enum Signal i32 { Hangup = 1, Interrupt = 2, Quit = 3, Terminate = 15 }

func Catch(sig Signal) !void      // start catching it
func WaitSignal() !Signal         // the next one, without holding a thread
func StopSignals()                // every wait answers NoSignals, for good
func Kill(pid u64, sig Signal) !void

func MaxFiles() u64               // descriptors this process may open
func RaiseMaxFiles(want u64) !u64 // 0 asks for the hard limit
func Pid() u64
func Cpus() u64
func Hostname() ?string

func Errno() i32                  // what the last C call left
func ClearErrno()
```

`Errno` is for an `extern` declaration that fails the way C does. errno belongs
to the thread, and a task that waits — on I/O, a lock, a channel, a sleep, the
end of a scope — may resume on another, so it is read straight after the call
and before anything that could wait. `ClearErrno` is for the functions that say
they failed only through errno, `strtol` and its family, whose answer means
nothing unless errno was zero before the call.

A signal handler may do almost nothing safely, so a signal arrives down a pipe
and `WaitSignal` is an ordinary task parked on the other end. That is the whole
of a graceful shutdown: wait, call `Close`, let the scope drain.

`StopSignals` is the other end of that. A task parked in `WaitSignal` has
nothing else to end it, and the scope that spawned it will not close until it
does; after this every wait answers `error.NoSignals` and keeps answering it.
The signals being caught go back to their default behaviour, so a program that
has stopped listening does not swallow a `SIGTERM`, and `Catch` afterwards is
an error rather than a way back.

Any signal number is catchable, not only the four the enum names:
`os.Catch(os.Signal(28))` asks for SIGWINCH.

`Args` fills an array you supply instead of allocating, and returns the part of
it that was used:

```sword
mut room := [8]string{}
args := os.Args(room[..])
```

### `std/time`

```sword
func Nanos(n i64) Duration
func Micros(n i64) Duration
func Millis(n i64) Duration
func Seconds(n i64) Duration

func (d Duration) AsNanos() i64
func (d Duration) AsMicros() i64
func (d Duration) AsMillis() i64
func (d Duration) AsSeconds() f64
func (d Duration) Add(other Duration) Duration
func (d Duration) Less(other Duration) bool

func Now() Instant                // monotonic: measure with this one
func Since(start Instant) Duration
func (t Instant) Add(d Duration) Instant
func (t Instant) Until() Duration // negative once it is past

func Unix() i64                   // wall clock seconds: stamp with this one
func UnixNanos() i64
func Sleep(d Duration)            // the scheduler is told first
```

A method can be called on a temporary — `time.Since(start).AsMillis()` — except
one that writes through its receiver, which needs a binding to keep the
change.

Wall-clock seconds are written as the two dates a server deals in, always in
UTC, into a slice the caller gives:

```sword
const Rfc3339Size = 20   // 2026-10-20T14:03:00Z
const HttpDateSize = 29  // Tue, 20 Oct 2026 14:03:00 GMT

func Rfc3339(unix i64, mut into []u8) !string
func HttpDate(unix i64, mut into []u8) !string

func ParseRfc3339(text string) !i64
func ParseHttpDate(text string) !i64
```

```sword
mut room := [32]u8{}
try res.Headers.Set("Date", try time.HttpDate(time.Unix(), room[..]))
```

`ParseRfc3339` reads any offset (`Z`, `-03:00`, `+05:30`) and drops a fraction
of a second. `ParseHttpDate` reads the three forms an HTTP recipient must
accept — the IMF-fixdate above, the obsolete RFC 850 form with a two-digit year
(69 and up is 19xx), and C's `asctime` — while `HttpDate` writes only the first,
the one a sender may use. Years run from 0000 to 9999; outside them a date is
`error.YearOutOfRange`, and a leap second, which Unix time cannot count, is
refused.

### `std/testing`

Tests live in `*_test.sword` beside the code and are found by name: `Test...`
taking one `*T`. See [Testing](testing.md).

```sword
func (t *T) Name() string
func (t *T) Failed() bool

// These record and carry on, and ask for no `try`.
func (mut t *T) Equal(got any, want any)
func (mut t *T) Same(got any, want any)        // Equal by another name
func (mut t *T) Check(ok bool)
func (mut t *T) Fail()
func (mut t *T) Failf(format string, args ...any)
func (mut t *T) Logf(format string, args ...any)

// These end the test, by returning an error the `try` carries out.
func (mut t *T) Must(got any, want any) !void
func (mut t *T) Require(ok bool) !void
func (mut t *T) Fatalf(format string, args ...any) !void
func (mut t *T) Skip(why string) !void
func (mut t *T) Run(name string, body func(mut *T) !void) !void
func (mut t *T) RunWith(name string, body func(mut *T) !void, arg any) !void
```

Tests run at the same time, 64 at once by default; `shield test <path> -p 1`
puts them back in order. `t.Mem` is an arena of that test's own. `t.Arg` is what `RunWith` handed the
subtest, since a body cannot capture anything.

Benchmarks are found the same way — `Benchmark...` taking one `*B` — and
`shield test <path> -bench` runs those instead of the tests:

```sword
func (b *B) Name() string
func (mut b *B) ResetTimer()            // setup does not count
func (mut b *B) StopTimer()
func (mut b *B) StartTimer()
func (mut b *B) Keep(v i64)             // so the work is not dropped
func (mut b *B) KeepFloat(v f64)
```

`b.N` is how many times the body should do its work, and the harness picks it:
it scales until a measurement lasts about a tenth of a second, then takes nine
and reports the median with half the quartile range as the spread. `b.Mem` is
an arena reset before every sample, and what the body takes from it after the
last `ResetTimer` is reported per operation.

### Channels

A queue tasks hand values through, and part of the language rather than a package
to import. See [Concurrency](concurrency.md#channels).

```sword
mut ch := try chan[T](&arena, capacity)  // capacity 0 is a handover
try ch <- v                              // waits while full; fails once closed
v := <-ch                                // ?T: waits while empty, nil once drained
for v := <-ch { }                        // until it closes
close(ch)                                // twice is harmless
```

`<-` is one token, so `a < -b` needs the space.

```sword
select {
case v := <-ch:      // v is ?T — nil when the channel closed
case <-ch:           // the same, thrown away
case ch <- value:    // when there is room
default:             // when none of them could go
}
```

At most one `default`, and a select with no cases at all is a compile error. A
receive case is ready when the channel has a value *or* is closed; a send case is
ready when there is room, and never on a closed channel. Cases are tried from a
rotating start, so one that is always ready cannot starve the rest. A select whose
only remaining cases send into closed channels stops the program rather than
waiting for what cannot happen.

The rest are ordinary methods:

```sword
func (c Chan[T]) TrySend(v T) bool      // false rather than a wait
func (c Chan[T]) TryRecv() ?T
func (c Chan[T]) Len() u64
func (c Chan[T]) Cap() u64              // 0 for a handover
func (c Chan[T]) Closed() bool
func (c Chan[T]) Free(mut a mem.Allocator)
```

A channel is a handle: copying one copies the handle, both copies are the same
channel, and it goes to a task by value. Its memory comes from the allocator you
pass, which is why there is no `make`.

No receiver is `mut`, because every task that shares a channel reaches it at once
and the state behind it is a `shared` value.

### `std/fs`

```sword
enum Mode i32 { Read, Write, Append }   // Write truncates or creates

func Open(path string, mode Mode) !File
func Of(fd i32) File                    // wrap one the process already has
func (f *File) Fd() i32                 // negative: a file nobody opened
func (f *File) Read(mut into []u8) !u64 // 0 means the end of the file
func (f *File) Write(from []u8) !void
func (f *File) WriteString(s string) !void
func (f *File) Close()

func Size(path string) ?u64             // nil when absent, or a directory
func Exists(path string) bool
func Remove(path string) !void
func MakeDir(path string) !void               // content if it already exists
func RemoveDir(path string) !void             // an empty one only
func ReadAll(path string, mut a mem.Allocator) ![]u8
func WriteAll(path string, data []u8) !void
func ReadStdin(mut a mem.Allocator, most u64) ![]u8

struct Entry { Name string; IsDir bool }
struct Info { Size u64; IsDir bool; ModifiedNanos i64; Mode u32 }

func Stat(path string) !Info
func Rename(from string, to string) !void
func ReadDir(path string, mut a mem.Allocator) ![]Entry
func TempPath(prefix string, mut into []u8) !string
```

`Stdin`, `Stdout` and `Stderr` are the descriptors the process starts with.

`Stat` answers what `Size` could not: missing, a directory, and cannot be read
were all the same nil. `Mode` is the permission bits and nothing else.

`ReadDir` leaves out `.` and `..`, gives names rather than paths, and copies
them into the allocator's memory so they outlive the walk. The order is
whatever the filesystem keeps.

`Rename` moves a file over whatever is already there, which is how a file is
saved without risking it — and `TempPath` is the other half. Its prefix is a
path, so the new file lands beside the one being replaced, in the same
filesystem, which is what makes the rename atomic:

```sword
mut room := [256]u8{}
scratch := try fs.TempPath("notes.txt.", room[..])
try fs.WriteAll(scratch, updated)
try fs.Rename(scratch, "notes.txt")
```

The file is made, not merely named: a path that is only unlikely to be taken is
a race the caller cannot see. `fs.WriteAll` on its own truncates first, so a
crash halfway through it loses what was there.

A file is never "not ready yet" — the wait is the disk, and no poller has
anything to say about it. So a file call goes to a thread kept for exactly that,
and the task waiting on it is put down like any other. Forty tasks reading at once
run on a pool of ten, not forty threads; `SWORD_IO_THREADS` is the cap and
`runtime.Read().IoThreads` is the count.

### `std/process`

Running another program, and talking to it.

```sword
struct Child { Pid u64; In fs.File; Out fs.File; Err fs.File }

func Start(path string, args []string, mut a mem.Allocator) !Child
func Run(path string, args []string, mut a mem.Allocator) !i32
func (mut c *Child) Wait() !i32
func (mut c *Child) Kill(sig i32) !void
func (mut c *Child) CloseIn()
func (mut c *Child) Close()
```

`args` is what comes after the program's own name. A path with no slash in it
is looked up in `PATH`, the way a shell would, and the child gets this
process's environment. The allocator holds `argv` while it is being built and
nothing after that.

`Start` gives the child a pipe on each of its three descriptors, which is the
part that matters: a language server is a conversation over the child's stdin
and stdout rather than a command whose output you collect. `Run` leaves it the
descriptors this process has and answers the status.

`Wait` answers the exit status, or 128 plus the signal when one killed it — the
shell's convention, and the only thing a single number can carry. Until
something waits, a child that has finished stays a zombie. `CloseIn` is how a
program reading until the end of its input is told that was all of it.

Starting a program and waiting for one both stop a thread and neither is
anything a poller can help with, so both go to the same pool as file I/O and
the task waiting is put down like any other.

### `std/c`

C's names for its types, so an `extern` declaration says what its header says.
Each is an alias, so a `c.Int` is an `i32` and passes for one without a
conversion.

```sword
extern func write(fd c.Int, buf c.Ptr, n c.SizeT) c.SSizeT
```

| C | `std/c` | is |
|---|---|---|
| `short`, `unsigned short` | `Short`, `UShort` | `i16`, `u16` |
| `int`, `unsigned int` | `Int`, `UInt` | `i32`, `u32` |
| `long`, `unsigned long` | `Long`, `ULong` | `i64`, `u64` |
| `long long`, `unsigned long long` | `LongLong`, `ULongLong` | `i64`, `u64` |
| `size_t`, `ssize_t`, `ptrdiff_t` | `SizeT`, `SSizeT`, `PtrDiffT` | `u64`, `i64`, `i64` |
| `float`, `double` | `Float`, `Double` | `f32`, `f64` |
| `_Bool` | `Bool` | `bool` |
| `char`, `signed char`, `unsigned char` | `Char`, `SChar`, `UChar` | `u8`, `i8`, `u8` |
| `const char *` | `String` | `[*]u8` |
| `void *` | `Ptr` | `[*]u8` |

The sizes are those of every platform Sword builds for, all of them 64-bit.
Whether a plain `char` is signed is the platform's choice: it is on x86-64 and
on Apple's arm64, not on Linux's arm64. `Char` is for text, where only the bytes
matter; a single `char` passed by value takes `SChar` or `UChar`, as the target
has it. A pointer C may answer with NULL is `?c.String` or `?c.Ptr`, and a
pointer to anything else stays `[*]T`.

### `std/dl`

Loading a library while the program runs. A library named by `extern "z"` has
to be there for the program to start; this is for one that may not be, or whose
name the program only learns while running — a plugin, an optional codec.

```sword
func NewLoader() Loader
func (mut l *Loader) Open(path string) !Library
func (mut l *Loader) Symbol(lib Library, name string) ![*]u8
func (mut l *Loader) Close(lib Library) !void
func (l *Loader) Why() string
func Suffix() string                 // ".so" or ".dylib"
```

```sword
mut ld := dl.NewLoader()
lib := ld.Open("libz.so.1") catch {
    try io.Printf("no zlib: {}\n", ld.Why())
    return 1
}
defer ld.Close(lib) catch {}
version := extern func() [*]u8(try ld.Symbol(lib, "zlibVersion"))
```

A path is taken as written, and a name without a slash is searched for the way
the system searches. `Open("")` is the program itself and everything already
loaded into it. Every symbol a library needs is bound when it is opened, so a
missing one is `Open`'s failure rather than a crash at its first call, and what
one library brings in does not leak into the lookups of the next.

The errors are `CannotOpen`, `NoSymbol` and `CannotClose`, and each has one
sentence. What the system said — which file it could not find, which symbol a
library wanted — is kept in the `Loader` for `Why()`, cleared at the start of
every call. It lives there rather than anywhere global because two loaders in
two tasks fail for two reasons.

`Symbol` answers the bare address, and converting it to an `extern func` is the
program's own promise about what is there. After `Close`, nothing found in the
library may be used: the code may be gone, and as with memory an allocator has
taken back, nothing checks. Opening and closing read files and run the
library's initialisers, so both go to the io pool and the task waiting is put
down like any other.

### `std/runtime`

What the scheduler is doing, for whoever runs the program rather than writes it.
A few relaxed loads, so it is cheap enough to read on a timer or to answer a
request with.

```sword
func Read() Stats
func (s Stats) Running() i64      // Started - Finished
```

| Field | |
|---|---|
| `Threads` | workers, including any hired to cover one stopped in the kernel |
| `Queued` | tasks spawned and not yet picked up — a queue that keeps growing is the first sign of a server taking more than it can serve |
| `Parked` | threads stopped inside a syscall that cannot be put down |
| `Stacks` | task stacks alive, in use or pooled |
| `Started` `Finished` | tasks begun and ended since the program did |
| `StackBytes` | what one task's stack reserves, which `SWORD_STACK_KB` sets |
| `IoThreads` | threads kept for calls that cannot be put down, capped by `SWORD_IO_THREADS` |

### `std/net`

TCP and UDP. `Listen` and `ListenUDP` bind loopback only; the `On` forms take
the address to bind. Port 0 asks the operating system to choose.

```sword
func Listen(port i32) !Listener                        // loopback only
func ListenOn(host string, port i32, share bool) !Listener
func (l *Listener) Port() i32
func (l *Listener) Accept() !Conn
func (l *Listener) Close()                   // wakes a blocked Accept

func Dial(host string, port i32) !Conn
func DialTimeout(host string, port i32, limit time.Duration) !Conn
func (c *Conn) SetTimeout(limit time.Duration) !void  // any one wait
func (c *Conn) SetDeadline(at time.Instant) !void     // the whole exchange
func (c *Conn) ClearDeadline() !void
func (c *Conn) Read(mut into []u8) !u64      // 0 means the peer is done
func (c *Conn) Write(from []u8) !void
func (c *Conn) WriteString(s string) !void
func (c *Conn) Peer(mut into []u8) !Peer   // { Address, Port }
func (c *Conn) Close()
```

A wait here puts the *task* down, not the thread: the descriptor goes to the
poller and the worker moves on. That is what lets connections outnumber threads
by a couple of orders of magnitude. See [Concurrency](concurrency.md#waiting).

`Stream` is what a socket satisfies without being asked, and what `std/http` is
written against, so a server serves a plain socket and a TLS connection with the
same code:

```sword
interface Stream {
    Read(mut into []u8) !u64
    Write(from []u8) !void
    WriteString(s string) !void
    SetTimeout(limit time.Duration) !void
    SetDeadline(at time.Instant) !void
    Peer(mut into []u8) !Peer
    Close()
}

func (c *Conn) Fd() i32                      // for a layer that wraps the socket
```

UDP sends datagrams: each arrives whole or not at all, in no promised order,
with no connection set up first. A socket made with `ListenUDP` talks to anyone
through `ReadFrom` and `WriteTo`; one made with `DialUDP` talks to one peer
through `Read` and `Write`, hears only from that peer, and can be told that
nobody is listening there. Both families work, IPv4 and IPv6.

```sword
func ListenUDP(port i32) !UDPConn                  // loopback only
func ListenUDPOn(host string, port i32) !UDPConn   // "" is every interface, v4 and v6
func DialUDP(host string, port i32) !UDPConn
func (c *UDPConn) ReadFrom(mut into []u8) !Datagram  // { Len, From, Truncated }
func (c *UDPConn) WriteTo(to Addr, from []u8) !void
func (c *UDPConn) Read(mut into []u8) !u64           // dialled: the peer's next datagram
func (c *UDPConn) Write(from []u8) !void
func (c *UDPConn) WriteString(s string) !void
func (c *UDPConn) SetTimeout(limit time.Duration) !void
func (c *UDPConn) SetDeadline(at time.Instant) !void
func (c *UDPConn) SetBroadcast(on bool) !void
func (c *UDPConn) Port() i32
func (c *UDPConn) Local() !Addr
func (c *UDPConn) Close()

func ParseAddr(ip string, port i32) !Addr    // numeric, "10.0.0.1" or "::1"
func Resolve(host string, port i32) !Addr    // a name, looked up without holding a thread
func Broadcast(port i32) Addr                // 255.255.255.255
func (a *Addr) Port() i32
func (a *Addr) IsV6() bool
func (a *Addr) Text(mut into []u8) !string   // 46 bytes holds any address
func (a *Addr) Equal(b Addr) bool
```

An `Addr` is a value of twenty bytes, so a server keeps the sender of every
datagram without allocating. A datagram longer than the buffer is cut: `Len` is
what fit, `Truncated` says so, and the rest is gone — `Read` makes that
`error.TooLong`. A dialled socket whose datagram found no one learns it on a
later read or write as `error.Refused`. The kernel refuses to send to a
broadcast address until `SetBroadcast(true)`, so no program floods a network by
accident.

A server that answers each datagram in capitals, and a client that asks it
twice:

```sword
import "std/io"
import "std/net"
import "std/time"

func serve(s *net.UDPConn, count u64) !void {
    mut buf := [512]u8{}
    for i in 0..count {
        got := try s.ReadFrom(buf[..])
        for j in 0..got.Len {
            if buf[j] >= 97 && buf[j] <= 122 {
                buf[j] -= 32
            }
        }
        try s.WriteTo(got.From, buf[0..got.Len])
    }
}

func ask(port i32) !void {
    mut c := try net.DialUDP("127.0.0.1", port)
    try c.SetTimeout(time.Seconds(2))   // an answer that is lost is not waited for forever
    mut buf := [512]u8{}
    for word in [2]string{"hello", "again"} {
        try c.WriteString(word)
        n := try c.Read(buf[..])
        try io.Printf("{} -> {}\n", word, string(buf[0..n]))
    }
    c.Close()
}

func main() !int {
    mut s := try net.ListenUDP(0)
    scope {
        spawn serve(&s, 2)
        spawn ask(s.Port())
    }
    s.Close()
    return 0
}
```
<!-- prints:
hello -> HELLO
again -> AGAIN
-->

### `std/tls`

TLS 1.2 and 1.3 over a socket, from OpenSSL. Optional: the build looks for OpenSSL
in the usual places (`SWORD_OPENSSL=<prefix>` names another, `SWORD_NO_TLS=1` skips
it), and a build without it answers `Available() == false` and fails every call
here rather than failing to link.

```sword
func Available() bool

func NewConfig() Config              // { CAFile, Verify, CertFile, KeyFile }
func ClientContext(c Config) !Context        // one context, any number of conns
func ServerContext(certFile string, keyFile string) !Context

enum Ask u8 { Nobody, Required, Optional }   // what a server asks of a client
func NewServerConfig(certFile string, keyFile string) ServerConfig
func ServerContextWith(c ServerConfig) !Context   // { CertFile, KeyFile, ClientCA, Clients }
func (c *Context) Free()

func Dial(ctx *Context, host string, port i32) !Conn
func Client(ctx *Context, socket net.Conn, host string) !Conn
func Server(ctx *Context, socket net.Conn) !Conn

func (c *Conn) Read(mut into []u8) !u64
func (c *Conn) Write(from []u8) !void
func (c *Conn) WriteString(s string) !void
func (c *Conn) SetTimeout(limit time.Duration) !void
func (c *Conn) SetDeadline(at time.Instant) !void
func (c *Conn) Peer(mut into []u8) !net.Peer
func (c *Conn) Version() string              // "TLSv1.3"
func (c *Conn) Cipher() string
func (c *Conn) PeerName(mut into []u8) ?string  // the peer's certificate subject
func (c *Conn) Verified() bool               // it presented one and it checked out
func (c *Conn) Close()

// A certificate signed by its own key, valid for a day, and marked as an authority
// so it can sign others. Development and tests.
func SelfSigned(host string, certFile string, keyFile string) !void
// One signed by that authority instead of by itself, so two of them can check each
// other. `forClient` marks it for client authentication.
func SignedBy(name string, caCert string, caKey string, certFile string,
              keyFile string, forClient bool) !void
```

`Ask.Required` refuses a client without a certificate signed by `ClientCA`, and is
the only setting under which a handler may believe `PeerName`. `Ask.Optional` lets
both kinds through and leaves the checking to the handler. Asking for a certificate
with no `ClientCA` to check it against is an error, not a default.

A `Conn` keeps the socket it took over in `Socket`, so deadlines and the peer's
address stay where they were, and it is a `net.Stream`. The handshake and every
read and write put the *task* down while the socket is not ready, the same as a
plain read — twenty handshakes at once are twenty tasks, not twenty threads.

`Config.Verify` off means encryption with no idea who is on the other end. It is
there for talking to your own machine and it is not security.

### `std/http`

HTTP/1.1 with keep-alive, routing, timeouts, chunked transfer encoding, static
files and TLS.

```sword
interface Handler {
    Serve(req *Request, mut res *Response) !void
}

func Listen(port i32) !Server                        // loopback only
func ListenOn(host string, port i32, share bool) !Server
func ListenTLS(port i32, certFile string, keyFile string) !Server
func ListenOnTLS(host string, port i32, share bool, certFile string,
                 keyFile string) !Server
func ListenWith(port i32, conf tls.ServerConfig) !Server      // asks for a client cert
func ListenOnWith(host string, port i32, share bool, conf tls.ServerConfig) !Server
func (s *Server) Secure() bool               // whether it handshakes first
func (mut s *Server) Free()                  // gives the certificate back
func (s *Server) Port() i32
func (s *Server) Serve(h Handler) !void      // up to 1024 connections at once
func (s *Server) ServeWith(h Handler, most u64) !void
func (s *Server) Live() u64                  // connections in flight
func (s *Server) Accepted() u64              // connections taken, in total
func (s *Server) Served() u64                // requests answered
func (s *Server) Failed() u64                // of those, handlers that failed
func (s *Server) Close()                     // stops accepting, then drains
```

A `Request` carries `Method`, `Target`, `Path`, `RawQuery`, `Proto`,
`RemoteAddr`, `PeerName`, `Headers` and `Body`. `PeerName` is the subject of the
client's certificate when the server asked for one and got a valid one, and empty
otherwise — including every request under `Ask.Optional` that arrived without one. A body sent with `Transfer-Encoding:
chunked` is decoded before the handler sees it. `Target` is the request line unchanged; `Path` and `RawQuery` are
its two halves. The strings point into the connection's read buffer, so they are
valid for as long as the handler runs.

```sword
func (r *Request) Param(name string) string  // what a {name} route matched
func (r *Request) Query(name string) string  // still percent-encoded
```

Routing. A `{name}` in a pattern matches one path segment and a `{name...}` at the
end matches the rest of the path, slashes and all — including none of it, so
`/files/{path...}` routes `/files/` too. A path that matched under another method
answers 405 rather than 404. Up to 64 routes, no allocation:

```sword
func NewMux() Mux
func (mut m *Mux) Handle(method string, pattern string, mut h Handler) !void
func (mut m *Mux) Get(pattern string, mut h Handler) !void
func (mut m *Mux) Post(pattern string, mut h Handler) !void
func (mut m *Mux) Put(pattern string, mut h Handler) !void
func (mut m *Mux) Delete(pattern string, mut h Handler) !void
```

Targets and query strings, decoding into space you provide:

```sword
func ParseTarget(target string) Target       // { Path, RawQuery }
func ParseURL(text string, fromHost string, fromPort i32) !URL  // { Host, Port, Target, TLS }
func QueryValue(query string, name string) string
func Unescape(s string, mut into []u8) !string   // %20 and + become a space
func Escape(s string, mut into []u8) !string
```

```sword
func (mut r *Response) Stream(status u64)          // switch to chunked
func (mut r *Response) Send(status u64, length u64)  // a length you know
func (mut r *Response) Printf(format string, args ...any)
func (mut r *Response) Text(status u64, s string)
func (mut r *Response) JSON(status u64, s string)
func (mut r *Response) SetHeader(name string, value string)
func (mut r *Response) Write(p []u8)
func (mut r *Response) WriteString(s string)
func (r *Response) Err() !void                     // the first write that failed
```

Both `Stream` and `Send` put the head out at once, so headers set after either are
too late, and every write from then on goes to the connection rather than into a
buffer. After `Send`, write exactly `length` bytes.

No write asks for `try`: the first that fails is kept, the rest do nothing, and
the server checks once the handler returns — a 500 if the head has not gone, the
connection closed if it has. `Err` is for a handler whose next step depends on
it, such as one feeding a stream to a client that may have left.

Files. A piece at a time against the length the file already has, so a large
download costs one buffer:

```sword
func ServeFile(mut res *Response, path string) !void
func NewFiles(root string) Files             // { Root, Index, Param }
func (f *Files) Serve(req *Request, mut res *Response) !void
func MimeType(path string) string            // by extension
```

`Files` serves what a `{path...}` route caught, under its root. A path containing
`..`, a NUL or a backslash is refused rather than resolved. A directory gets
`Index` — `index.html` by default, and an empty `Index` makes directories 404.
Only GET and HEAD; anything else is a 405. Both send `Last-Modified` and a weak
`ETag` built from the size and modification time; `Files` answers
`If-None-Match` and `If-Modified-Since` with 304, the tag taking precedence.

Header lookup is case-insensitive. A request carries at most 32 headers and
16 KiB of head; beyond that the connection is rejected. An accepted connection
gets a 15-second timeout, so a client that connects and says nothing releases
its accept loop rather than holding it.

The client returns a response whose body lives in the allocator you pass in:

```sword
func NewClient() Client                      // 10s connect, 30s read
func (c *Client) Get(host string, port i32, target string,
                     mut a mem.Allocator) !ClientResponse
func (c *Client) Post(host string, port i32, target string,
                      contentType string, body []u8,
                      mut a mem.Allocator) !ClientResponse
func (c *Client) Do(host string, port i32, method string, target string,
                    contentType string, body []u8,
                    mut a mem.Allocator) !ClientResponse

// Over TLS. Fetch takes a URL, which is the only shape that can say a scheme.
func (c *Client) Fetch(url string, mut a mem.Allocator) !ClientResponse
func (c *Client) GetTLS(host string, port i32, target string,
                        mut a mem.Allocator) !ClientResponse
func (c *Client) PostTLS(host string, port i32, target string,
                         contentType string, body []u8,
                         mut a mem.Allocator) !ClientResponse
func (c *Client) DoTLS(host string, port i32, method string, target string,
                       contentType string, body []u8,
                       mut a mem.Allocator) !ClientResponse

// The same through a default client.
func Get(host string, port i32, target string,
         mut a mem.Allocator) !ClientResponse
func Post(host string, port i32, target string, contentType string, body []u8,
          mut a mem.Allocator) !ClientResponse
func Fetch(url string, mut a mem.Allocator) !ClientResponse
```

`Client` has two `time.Duration` fields, `Connect` and `Read`, either of which
may be zero to wait as long as the kernel would, and `Redirects`, how many to
follow — five by default, zero to hand the 3xx back. 301, 302 and 303 become a
GET; 307 and 308 keep the method and body.

```sword
func (mut c *Client) Close()            // lets go of the kept connection
```

A chunked reply is decoded, and a reply with no framing at all is read until the
connection closes. `Client.TLS` is a `tls.Config` and decides how an `https` URL is
trusted.

A client **keeps the connection it used**, with its TLS session, and uses it again
for the next request to the same host and port. That is why the methods take `mut`
and why a client belongs to one task. `Close()` matters: a kept connection holds a
task on the server. A request that fails on the kept connection is retried on a
fresh one for `GET` and `HEAD`, and comes back as `error.Interrupted` otherwise —
the request may already have been carried out.

## Command line

```
shield <file.sword | directory> [options]
shield test <file.sword | directory> [options]

  -o <path>       output binary, default a.out
  -I <dir>        another directory to search for packages
  --link <arg>    an object, a library or a linker option; one per --link
  --static        link the libraries `extern` names from their archives
  --target=<t>    build for another machine, a 64-bit LLVM triple
  -c              stop at the object file, linking nothing
  --mode=<m>      debug | safe | fast | small, default safe
  -O<level>       override the optimisation level
  --split         one object per package, kept for the next build
  -p <n>          test only: how many tests may run at once
  -run <name>     test only: run just this one; repeat for more
  -bench          test only: run the Benchmark... functions instead
  -sim            test only: every test under simulated scheduling, once a seed
  -seeds <n>      test only, with -sim: how many seeds, default 100
  -seed <n>       test only, with -sim: just this seed
  --sim           switch tasks at atomics too, under SWORD_SIM_SEED
  --emit-tokens   stop after lexing
  --emit-ast      stop after parsing and checking
  --emit-ir       stop after lowering, print Sword IR
  --emit-llvm     print the LLVM IR
  --emit-link     print what the linker gets after the program's object
```

A `.sword` file compiles alone. A directory compiles as one package.

A build hands the whole program to LLVM as one module, so a small function from
the standard library is inlined into the loop that calls it. `--split` compiles
each package to an object of its own instead, kept in `~/.sword/build` (under
`SWORD_CACHE` when that is set) by the hash of its code and of how it was
compiled, and only what changed goes through LLVM again. Rebuilding a small
server after editing only its own package takes well under half the time; a
program that imports next to nothing has nothing to gain. The price
is that inlining stops at the edge of a package, and a hot loop around a small
call into another one can run several times slower. It is meant for the
edit-and-run cycle, not for the binary you ship. An object nothing has used for
thirty days is removed by a later build, and `shield clean` removes them all.

A library named by `extern "name"` is linked without being mentioned here (see
[C libraries](#c-libraries)). `--link` reaches the linker for the rest — an
object of the program's own, a library somewhere unusual, a shim around C that
`extern` alone cannot describe, such as a function taking a struct whose layout
differs between systems. One argument each, in the order given, after the
program's own object and before the declared libraries:

```
shield sheath.sword -o sheath --link shim.o --link -L/opt/homebrew/lib \
    --link -lsomething
```

`--target` takes an LLVM triple, `x86_64-unknown-linux-gnu` or
`aarch64-apple-macos`, and passes it to clang. Sword lays memory out in 64-bit
words — every pointer, length and `usize` is eight bytes before LLVM sees the
program — so only a 64-bit, little-endian machine is accepted; a narrower one
would get every layout wrong. Linking needs a runtime built for the machine
(see [Installing](install.md#a-runtime-for-another-machine)). `-c` stops at the
object file instead, and needs nothing but clang: it is how a program goes
somewhere this machine cannot link for, and the rest of the line comes from
`--emit-link`.

A build that links by its own means — through `--emit-llvm` and a Makefile, say
— asks `--emit-link` for the rest of the line: what the runtime was built
against, `--link`, the declared libraries and what weak functions need, one
argument a line, to go after the program's object and the runtime archive.

`shield test` builds the package together with its `*_test.sword` files behind a
generated entry point, runs it, and hands back its exit status. Those files are
left out of every other build. See [Testing](testing.md).

`SWORD_SIM_SEED=<n>` runs any program on one thread with every scheduling
choice drawn from the seed and a virtual clock; the same seed makes the same
run. It exits with 3 on a deadlock and 4 when the program does something the
simulator does not simulate yet. See [Simulation](testing.md#simulation).

| Mode | Bounds and overflow checks | Optimisation |
|---|---|---|
| `debug` | yes | none |
| `safe` | yes | full |
| `fast` | no | full |
| `small` | no | for size |

Shift checks go with bounds and overflow checks. A division is checked in every
mode.

## Environment

| | |
|---|---|
| `SWORD_ROOT` | where the compiler looks for the standard library |
| `SWORD_THREADS` | fixed worker count, defaults to one per core |
| `SWORD_MAX_THREADS` | how far the pool may grow to cover blocked tasks, default 512 |
| `SWORD_STACK_KB` | stack per task in KiB, default 1024, held between 64 and 262144 |
| `SWORD_IO_THREADS` | threads for file I/O and name resolution, default the core count or 4 |

## Reserved but not implemented

Nothing, now that `chan` is a package rather than a keyword.
