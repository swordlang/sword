# A command-line tool

A small grep: a pattern, an optional file, three flags, and the exit statuses
scripts expect. Run with no arguments, as it is here, it prints its usage to
standard error and exits with 2.

Uses: [errors](../tour.md#errors), [the outside world](../tour.md#the-outside-world),
[`switch`](../reference.md#switch), [`std/os`](../reference.md#stdos),
[`std/fs`](../reference.md#stdfs).

```sword
import "std/fmt"
import "std/fs"
import "std/io"
import "std/mem"
import "std/os"
import "std/strings"

const Usage = "usage: grep [-i] [-n] [-c] pattern [file]\n"

// grep's own exit statuses: a line matched, none did, or something went wrong.
const Found = 0
const NotFound = 1
const Trouble = 2

struct Options {
    ignoreCase bool
    numbers    bool
    countOnly  bool
    pattern    string
    path       string     // empty: standard input
}

error BadFlag = "an option grep does not know"
error NoPattern = "no pattern given"

func parse(args []string) !Options {
    mut o := Options{ignoreCase: false, numbers: false, countOnly: false, pattern: "", path: ""}
    mut rest u64 = 0
    for arg in args {
        if rest == 0 && arg.len > 1 && arg[0] == "-"[0] {
            switch arg {
            case "-i":
                o.ignoreCase = true
            case "-n":
                o.numbers = true
            case "-c":
                o.countOnly = true
            default:
                return error.BadFlag
            }
            continue
        }
        if rest == 0 {
            o.pattern = arg
        } else if rest == 1 {
            o.path = arg
        }
        rest += 1
    }
    if rest == 0 {
        return error.NoPattern
    }
    return o
}

func contains(line string, pattern string, ignoreCase bool) bool {
    if !ignoreCase {
        return strings.Contains(line, pattern)
    }
    if pattern.len > line.len {
        return false
    }
    for at in 0..line.len - pattern.len + 1 {
        if strings.EqualFold(line[at..at + pattern.len], pattern) {
            return true
        }
    }
    return false
}

func run() !int {
    mut room := [16]string{}
    args := os.Args(room[..])
    o := parse(args[1..args.len]) catch |e| {
        try io.Fail(e.Message())
        try io.Fail("\n")
        try io.Fail(Usage)
        return Trouble
    }

    mut sys := mem.NewSystem()
    mut text := []u8("")
    if o.path.len == 0 {
        text = try fs.ReadStdin(&sys, 1 << 26)
    } else {
        text = fs.ReadAll(o.path, &sys) catch |e| {
            try io.Errorf("grep: {}: {}\n", o.path, e.Message())
            return Trouble
        }
    }
    input := string(text)

    mut out := io.NewWriter(1)
    mut matched u64 = 0
    mut number u64 = 0
    mut start u64 = 0
    for start < input.len {
        end := start + strings.IndexByte(input[start..input.len], "\n"[0])
        line := input[start..end]
        number += 1
        if contains(line, o.pattern, o.ignoreCase) {
            matched += 1
            if !o.countOnly {
                if o.numbers {
                    try fmt.U64(&out, number)
                    try out.WriteByte(":"[0])
                }
                try out.Write([]u8(line))
                try out.WriteByte("\n"[0])
            }
        }
        start = end + 1
    }
    if o.countOnly {
        try fmt.U64(&out, matched)
        try out.WriteByte("\n"[0])
    }
    try out.Flush()
    if matched == 0 {
        return NotFound
    }
    return Found
}

func main() int {
    return run() catch |e| {
        io.Errorf("grep: {}\n", e.Message()) catch {}
        return Trouble
    }
}
```
<!-- exits: 2 -->

```
$ printf 'Sword is a language\nthe race checker\nSWORD again\n' > in.txt
$ ./grep -n -i sword in.txt
1:Sword is a language
3:SWORD again
$ ./grep -c race < in.txt
1
$ ./grep zebra in.txt; echo $?
1
$ ./grep a nofile; echo $?
grep: nofile: the file could not be opened
2
```

## Arguments

`os.Args` fills an array the program gives it and returns the part it used, so
reading the command line allocates nothing. Sixteen is plenty for this tool;
arguments past that are not seen.

`parse` walks them once. Anything starting with `-` before the pattern is a
flag, and a `switch` on a string compares contents, so each flag is a `case`.
An unknown one is an error of its own, `BadFlag`, with a message that says
what went wrong; so is a missing pattern. `run` catches either, prints the
message and the usage to standard error, and answers 2.

## Exit statuses

```sword
const Found = 0
const NotFound = 1
const Trouble = 2
```

These are grep's, and every shell script that calls grep relies on them: 0 when
a line matched, 1 when none did, 2 when something went wrong. `main` returns an
`int`, and that is the status the process exits with. Named constants say which
is which at every `return`.

`run` is `!int`, so inside it a `try` can stop at any failure, and `main`
turns whatever escaped into a message and a 2. That keeps the program from
exiting with the error's own code, which a script would read as something it
is not.

## Reading and matching

The whole input is read at once, from the file or from standard input, with a
limit of 64 MiB. Lines are slices of it, found with `strings.IndexByte`, which
answers the length of the text when there is no newline; the last line needs
no special case.

`-i` compares with `strings.EqualFold` at each position, which ignores the case
of ASCII letters. Output goes through an `io.Writer`, buffered and flushed once
at the end, so a large file is not a system call per line.
