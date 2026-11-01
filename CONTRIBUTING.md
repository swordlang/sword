# Contributing to Sword

Sword is young, and there is room to help in every part of it: the compiler, the
runtime, the standard library, the documentation and the editors. This page says
how a change gets in.

## Where to start

**Found a bug?** Open an issue with the smallest program that shows it, what you
expected, what happened, and where you ran it: the commit you built
(`git rev-parse --short HEAD`), the operating system and the processor. A program
the compiler accepted and should have refused counts as a bug as much as a crash
does, and for the race checker it is the more important kind.

**Want to change the language?** Syntax, semantics, or what the checker accepts
starts as an issue, not a pull request. Describe the problem with code: what you
have to write today, and what you would write instead. The design decisions are
the maintainer's, and a change to the language is merged only once its issue has
been agreed on — that conversation is cheaper than a pull request nobody can
take. Some things are settled, because they are why the language exists: nothing
allocates implicitly, a task's lifetime is a lexical scope, a binding is
immutable unless it says `mut`, and the compiler refuses a program in which two
tasks can touch the same memory at once. [Why Sword](docs/why.md) explains what
they buy.

**Everything else** — a fix, a function the standard library is missing, a
clearer page of documentation, support for another editor — can come straight
as a pull request. For anything large, an issue first saves work on both sides.

## Building and testing

[docs/install.md](docs/install.md) lists what the build needs. Then:

```sh
make
make test
```

`make test` has to pass. It runs the compiler's tests in `tests/`, the language
server's, the standard library's, and the editors' indentation for whichever of
vim, Neovim and Emacs you have installed.

A fix comes with a test that fails without it. A compiler test is a file in
`tests/` whose first comment says what should happen:

```sword
// expect: 0
// expect-output: hello
import "std/io"

func main() !int {
    try io.Println("hello")
    return 0
}
```
<!-- prints:
hello
-->

`// expect-error: <message>` instead says the program must not compile, and with
what error. A change to the standard library gets a test in the package's own
`_test.sword` file; `shield test std/<package>` runs it.

A change to the runtime, or to anything concurrent, needs more than one run.
`tests/sanitize.sh address` builds the runtime under AddressSanitizer and
UndefinedBehaviorSanitizer and runs every test against it;
`tests/sanitize.sh thread` does the same with ThreadSanitizer over the tests
where tasks meet. Then run the concurrency tests many times with
`SWORD_THREADS` at 1, 2, 4, 8 and 16.
A test that fails one time in twenty is a bug until shown otherwise: the run
that failed is usually the one telling the truth. Say in the pull request what
you ran.

The VS Code extension has its own test, which downloads a copy of VS Code the
first time:

```sh
cd editors/vscode
npm install
npm test
```

## What a pull request is checked for

Every pull request runs, on Linux and macOS:

- `make test`, with TLS and without, and `tests/threads.sh`, which runs the
  whole suite at 1, 2, 3, 4, 8 and 16 threads and requires the same answers;
- `tests/sanitize.sh` with both sanitizers;
- `tests/docs.sh`, which builds every example program in the documentation;
- that each commit's subject is in the form described under Commits below;
- CodeQL over the compiler, the runtime and the workflows, and a review of any
  dependency the change brings in;
- the VS Code extension's test, when the extension or the language server
  changed.

A pull request from somebody's first contribution waits for the maintainer to
start its checks. Each script runs the same way on your machine, and running it
first is quicker than waiting for the checks to say the same.

## Writing the change

Match the code around it: its names, how much it comments, how it is laid out.
A comment says why, not what the next line does. Code, comments and
documentation are in English.

The language's rules apply to its own library. A function in `std` that touches
the heap takes an `Allocator`, like everyone else's.

Documentation is part of the change. A new feature, flag or library function
updates the page that describes it — the [reference](docs/reference.md), the
[tour](docs/tour.md) or the guide for that area — written for somebody learning
the language.

## Commits

Each commit is one coherent change, and each builds and passes the tests on its
own. Messages follow the conventional form — `feat(scope):`, `fix(scope):`,
`docs:`, `test:`, `perf:`, `refactor:`, `ci:`, `build:`, `chore:` — with a body that says why the
change was made and what it cost. A number in a commit message or in the
documentation is one you measured; say how.

## Branches

Two branches live forever, and nobody pushes to either of them: both take
changes only through a pull request whose checks have passed.

| | |
|---|---|
| `main` | released versions only. Each merge into it is tagged `vX.Y.Z`. |
| `develop` | where finished work is integrated. Pull requests go here. |

Everything else is short-lived, starts from `develop`, and is named for what it
does, with the same words commits use:

| | |
|---|---|
| `feat/<name>` | something the language or the library can do that it could not |
| `fix/<name>` | a bug |
| `perf/<name>` | faster or smaller, measured |
| `refactor/<name>` | the same behaviour, arranged better |
| `docs/<name>` | documentation only |
| `test/<name>` | tests only |
| `ci/<name>`, `chore/<name>` | the build, the workflows, housekeeping |

`<name>` is a few lower-case words joined by hyphens — `feat/udp`,
`fix/poller-close` — with the issue number in front when there is one:
`fix/19-string-concat`. A pull request is merged with a merge commit rather than
squashed, since each commit already builds and passes on its own and says why,
and the branch is deleted once it is in.

A release is prepared on `release/X.Y.Z`, cut from `develop`: only fixes land
there, and it goes to `main` through a pull request, is tagged, and is merged
back into `develop`. A fix that cannot wait for the next release is
`hotfix/<name>`, cut from `main` and merged into both. The VS Code extension is
released separately, by a `vscode-vX.Y.Z` tag.

## Where things are

| | |
|---|---|
| `src/` | the compiler, `shield`: lexer, parser, checker, lowering, LLVM output |
| `lsp/` | the language server, `swordls`, sharing the compiler's front end |
| `rt/` | the runtime: scheduler, tasks, the poller, networking, TLS |
| `std/` | the standard library, written in Sword |
| `tests/` | the compiler's tests, and the scripts that run every suite |
| `editors/` | vim, Neovim, Emacs and VS Code |
| `docs/` | the documentation |

## Security

A security problem is reported privately, not in an issue; [SECURITY.md](SECURITY.md)
says how and what counts.

## License and conduct

Sword is under the [MIT License](LICENSE), and so is what you contribute to it:
by sending a pull request you agree that your change is released under the same
terms.

Everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
