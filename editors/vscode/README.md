# Sword for Visual Studio Code

Support for [Sword](https://github.com/swordlang/sword), a compiled systems
language with Go's syntax and Zig's memory model, whose compiler refuses a
program where two tasks can touch the same memory at once.

## What you get

**Errors as you type.** The same ones `shield` reports, from the language
server, `swordls`. It analyses the whole package rather than the open file, so a
name declared in a sibling file is a name, not an error, and a file you have not
saved is checked as it is on screen.

**Colour by what a name is.** The server says whether a word is a type, a
function, a parameter or a package, and VS Code paints it with your own theme. A
TextMate grammar colours the file before the server answers.

**Completion.** After `mem.` you get what the package exports, after `conn.` that
value's fields and methods, after `error.` the errors your program declares.

**Running.** *Sword: Run File* builds the file and runs it in a terminal, from a
temporary directory so nothing is left in your workspace. *Sword: Run Tests in
Package* runs `shield test` on the file's package, and each `func Test...` in a
`_test.sword` file has a **run test** link above it that runs that test alone.
Both are in the ▷ menu at the top right of a Sword editor.

**Snippets** for the shapes you write most: `func`, `method`, `main`, `test`,
`scope`, `lock`, `switch`, `select` and `error`.

**Indentation.** Enter goes one level in after an open bracket; a closing
bracket, `case` and `default` come back out; Enter inside `/* */` carries the
column of stars on.

## Requirements

Sword itself, installed so that `shield` and `swordls` are on your PATH:

```sh
git clone https://github.com/swordlang/sword
cd sword
make
make install
```

That puts both in `~/.local/bin`. The
[install guide](https://github.com/swordlang/sword/blob/main/docs/install.md)
has the details and what the build needs; Sword builds on macOS and Linux.

Without `swordls` the extension says so once and keeps the grammar's colouring.
Without `shield`, running something says so.

## Settings

| Setting | Default | |
|---|---|---|
| `sword.serverPath` | `swordls` | The language server: a name looked up on PATH, or a path |
| `sword.shieldPath` | `shield` | The compiler, used to run programs and tests |

## Known limits

VS Code indents by rules that look at one line at a time. A line carried over
from the one above — the rest of a long call, or of a condition — goes one level
in rather than lining up under the bracket or the condition, as vim and Emacs do
with Sword. Align those by hand.

## Learning the language

The [tour](https://github.com/swordlang/sword/blob/main/docs/tour.md) starts
at hello world and gets to parallel code, and every section is a program you can
run.
