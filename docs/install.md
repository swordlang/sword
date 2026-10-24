# Installing Sword

## What you need

A C++17 compiler and `clang` on your PATH. That is the whole list; OpenSSL is
optional and only for TLS.

`clang` is not a build-time dependency — it is a runtime one. Sword compiles to
LLVM IR and hands that text to `clang` to assemble and link, so `clang` has to
be there whenever you compile a Sword program, not just when you build the
compiler.

On macOS the Xcode command line tools give you both. On Debian or Ubuntu,
`apt install clang build-essential`.

### OpenSSL, for TLS

`std/tls` and `http.ListenTLS` need it. The build looks in the usual places —
`/opt/homebrew/opt/openssl@3`, `/usr/local/opt/openssl@3`, `/usr/include` — and
builds without TLS if it finds nothing, in which case `tls.Available()` answers
false and every call in that package fails with `error.NoTLS`. Nothing else is
affected, and no program fails to link.

```sh
brew install openssl@3                  # macOS
apt install libssl-dev                  # Debian, Ubuntu

make SWORD_OPENSSL=/path/to/openssl     # somewhere else entirely
make SWORD_NO_TLS=1                     # deliberately without
```

Whatever it decides ends up in `libsword_rt.flags` next to the archive, which is
what the compiler reads to link your programs. Switching the decision and running
`make` again is enough; no `make clean` needed.

## Building

```sh
git clone https://github.com/swordlang/sword
cd sword
make
```

That produces three things:

| | |
|---|---|
| `shield` | the compiler |
| `swordls` | the language server, for editors |
| `libsword_rt.a` | the scheduler, linked into programs that use tasks |
| `libsword_rt.flags` | what else to link it against, read by the compiler |

You can stop here and run `./shield` straight out of the tree. Everything it
needs it finds next to itself.

## Installing

```sh
make install
```

Installs into `~/.local` by default, so no `sudo`:

```
~/.local/bin/shield
~/.local/bin/swordls
~/.local/lib/sword/libsword_rt.a
~/.local/share/sword/std/
~/.local/share/sword/editors/
```

Pick somewhere else with `PREFIX`:

```sh
make install PREFIX=/usr/local     # needs sudo for this one
make install PREFIX=/opt/sword
```

`make uninstall` takes it back out, and honours the same `PREFIX`.

If your shell cannot find `shield` afterwards, `~/.local/bin` is not on your
PATH. Add it:

```sh
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc   # or ~/.bashrc
```

## Checking it works

```sh
cat > hello.sword <<'EOF'
import "std/io"

func main() !int {
    try io.Print("hello\n")
    return 0
}
EOF

shield hello.sword -o hello
./hello
```

If that prints `hello`, the compiler found its standard library and the
linker found the runtime.

## Where the compiler looks for things

`shield` locates the standard library and the runtime archive relative to
itself, so both the build tree and an installed tree work without
configuration. It checks, in order:

1. `$SWORD_ROOT`, if you set it
2. next to the binary — the build tree
3. `../share/sword` and `../lib/sword` — an installed tree

`SWORD_ROOT` is the escape hatch if you put things somewhere unusual:

```sh
SWORD_ROOT=/opt/sword/share/sword shield program.sword -o program
```

Imports also resolve relative to the file being compiled, which is how a
program finds packages that live beside it.

## A runtime for another machine

`shield --target=<triple>` builds for another machine, and links against a
runtime built for it. `make TARGET=<triple>` builds one beside this machine's own,
as `libsword_rt-<triple>.a`, and `make install` installs it with the rest:

```sh
make TARGET=x86_64-apple-macos                    # another Apple architecture
make TARGET=aarch64-linux-gnu SYSROOT=/path/to/sysroot
```

clang is a cross compiler by nature. What it needs from the other machine is
its C and C++ headers and libraries — a sysroot — unless they are this
machine's own, which is the case for the other Apple architecture. Linking a
program for another machine needs the same sysroot, and a linker that can
write for it:

```sh
shield app.sword --target=aarch64-linux-gnu -o app \
    --link --sysroot=/path/to/sysroot --link -fuse-ld=lld
```

The runtime switches between tasks in a few lines of assembly written for
x86-64 and aarch64, so those are the machines it builds for, and it is built
without TLS: where OpenSSL for another machine lives is not something to guess.

## Editors

`swordls` speaks LSP over stdin and stdout. It gives you diagnostics as you
type and semantic tokens, which let the editor colour a name by what it
actually is — a type, a function, a parameter — using your own colour scheme.

Setup for Neovim, Vim, VS Code and Emacs is in [`editors/`](../editors/README.md).
The short version for Neovim:

```lua
vim.opt.runtimepath:append(vim.fn.expand('~/.local/share/sword/editors/vim'))
dofile(vim.fn.expand('~/.local/share/sword/editors/nvim/sword.lua')).setup{}
```

## Building the compiler with sanitizers

Worth knowing if you plan to work on the compiler itself:

```sh
make clean
make CXXFLAGS="-std=c++17 -g -O1 -fsanitize=address,undefined -Wall -Wextra"
make test
```

That checks the compiler. The runtime is linked into the programs it compiles,
so it is checked through them: `tests/sanitize.sh address` builds a copy of the
runtime under AddressSanitizer and UndefinedBehaviorSanitizer and runs every
test program against it, and `tests/sanitize.sh thread` does the same under
ThreadSanitizer for the programs where tasks meet. Both need clang.

`make test` runs four suites: the language tests in `tests/*.sword`, each
declaring its expected exit status or compile error in a comment; an LSP test
that drives the real server over stdio; the standard library's own tests; and
the editors' indentation, for whichever of vim, Neovim and Emacs is installed.
