#!/bin/sh
# The whole suite at several pool sizes, compared. A program's answer is not
# supposed to depend on how many threads ran it, and this is the check that it
# does not: every runnable test is compiled once and run at each count, and has
# to exit the same way and print the same bytes every time. A test that reports
# a clock or the scheduler's own counters says so with
# `// output-varies: <why>` and is held to its exit status alone.
#
# The standard library's tests run at each count too; they print their own
# timings, so there it is the verdict that has to agree.
#
# With SWORD_ANSWERS set, each test's exit status and checksum go to that file
# as well, one line each, for CI to compare between machines. A test whose
# output is meant to differ from one system to another, because it prints
# something the operating system words, says so with
# `// output-differs: <why>` and is compared there by its exit status alone.
#
#   SWORD_COUNTS  the pool sizes to try   (default: 1 2 3 4 8 16)
#   SWORD_ROUNDS  runs at each size       (default: 1)
#   SWORD_ANSWERS where to write the answers (default: nowhere)
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
shield="$root/shield"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

counts=${SWORD_COUNTS:-1 2 3 4 8 16}
rounds=${SWORD_ROUNDS:-1}

pass=0
fail=0

for src in "$root"/tests/*.sword; do
    grep -q '^// expect:' "$src" || continue
    name=$(basename "$src" .sword)
    want_exit=$(sed -n 's|^// expect: *||p' "$src" | head -1)
    varies=$(sed -n 's|^// output-varies: *||p' "$src" | head -1)
    differs=$(sed -n 's|^// output-differs: *||p' "$src" | head -1)

    if ! "$shield" "$src" -o "$tmp/$name" > "$tmp/$name.log" 2>&1; then
        echo "FAIL $name: compilation failed"
        sed 's/^/     /' "$tmp/$name.log"
        fail=$((fail + 1))
        continue
    fi

    : > "$tmp/$name.answers"
    for threads in $counts; do
        round=0
        while [ "$round" -lt "$rounds" ]; do
            # Braced so the shell's own note about a program that aborted on
            # purpose goes where the program's stderr went.
            { SWORD_THREADS=$threads perl -e 'alarm 60; exec @ARGV' \
                "$tmp/$name" > "$tmp/$name.out"; } 2>/dev/null
            got=$?
            if [ -n "$varies" ]; then
                sum=-
            else
                sum=$(cksum < "$tmp/$name.out")
            fi
            echo "$got $sum" >> "$tmp/$name.answers"
            printf '%2s: exit %s, ' "$threads" "$got" >> "$tmp/$name.seen"
            head -c 200 "$tmp/$name.out" | tr '\n' ' ' >> "$tmp/$name.seen"
            echo >> "$tmp/$name.seen"
            round=$((round + 1))
        done
    done

    answers=$(sort -u "$tmp/$name.answers" | wc -l | tr -d ' ')
    first=$(head -1 "$tmp/$name.answers" | cut -d' ' -f1)
    if [ -n "${SWORD_ANSWERS:-}" ]; then
        if [ -n "$differs" ]; then
            echo "$name $first -" >> "$SWORD_ANSWERS"
        else
            echo "$name $(head -1 "$tmp/$name.answers")" >> "$SWORD_ANSWERS"
        fi
    fi
    if [ "$answers" != 1 ] || [ "$first" != "$want_exit" ]; then
        echo "FAIL $name: $answers answers across thread counts, want exit $want_exit"
        sed 's/^/     /' "$tmp/$name.seen"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
done

# A package, not a file: one with several test files is still tested once.
for pkg in "$root"/std/*/; do
    pkg=${pkg%/}
    set -- "$pkg"/*_test.sword
    [ -e "$1" ] || continue
    name=std/$(basename "$pkg")
    bad=
    for threads in $counts; do
        round=0
        while [ "$round" -lt "$rounds" ]; do
            if ! SWORD_THREADS=$threads perl -e 'alarm 120; exec @ARGV' \
                    "$shield" test "$pkg" > "$tmp/std.out" 2>&1; then
                bad="$bad $threads"
                sed 's/^/     /' "$tmp/std.out" > "$tmp/std.failed"
            fi
            round=$((round + 1))
        done
    done
    if [ -n "$bad" ]; then
        echo "FAIL $name: failed at$bad threads"
        cat "$tmp/std.failed"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
done

echo "$pass passed, $fail failed at $counts threads"
[ "$fail" -eq 0 ]
