#!/bin/sh
# tests/docs.sh — every example program in the documentation compiles, and the
# ones that say what they do still do it.
#
# A ```sword block with a `func main` in it is a whole program, and a reader
# will paste it; it has to build as written. A block that imports a package of
# the reader's own, rather than one from std, is showing how packages fit
# together and cannot be built on its own, so it is left out.
#
# Right after the closing fence, an HTML comment (invisible on the page) may
# say what running the program does:
#
#   <!-- prints:
#   what it writes to standard output, exactly
#   -->
#   <!-- exits: 3 -->
#
# A program with either is run, with nothing on its standard input, and held to
# it; the exit status is 0 unless `exits` says otherwise. Leave the markers off
# a program whose answer depends on the clock, the network, the environment or
# files outside it — it is still compiled. A marker anywhere else is an error, so one cannot be
# left behind when its example moves.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
shield=$root/shield
out=$(mktemp -d "${TMPDIR:-/tmp}/sword-docs.XXXXXX")
trap 'rm -rf "$out"' EXIT

pass=0
fail=0
skip=0
ran=0
for page in "$root"/docs/*.md "$root"/README.md "$root"/CONTRIBUTING.md \
        "$root"/editors/README.md "$root"/editors/vscode/README.md; do
    [ -f "$page" ] || continue
    # One file per block, named for the page and the line the block starts on,
    # with .out and .exit beside it when the page says what it does.
    awk -v dir="$out" -v page="$(basename "$page" .md)" '
        function stray() {
            print page ".md:" NR ": a prints/exits marker that does not follow a program" > (dir "/stray")
        }
        printing {
            if ($0 == "-->") { printing = 0; after = 1; close(outfile); next }
            print > outfile
            next
        }
        after && /^<!-- exits: *[0-9]+ *-->$/ {
            code = $0; gsub(/[^0-9]/, "", code)
            print code > (base ".exit"); close(base ".exit")
            next
        }
        after && /^<!-- prints:$/ {
            outfile = base ".out"; printf "" > outfile
            printing = 1; after = 0
            next
        }
        { after = 0 }
        /^<!-- (prints|exits):/ { stray(); next }
        /^```sword[ \t]*$/ { inside = 1; start = NR; body = ""; next }
        /^```/ && inside {
            inside = 0
            if (body ~ /func main\(/) {
                base = dir "/" page "_" start
                printf "%s", body > (base ".sword")
                close(base ".sword")
                after = 1
            }
            next
        }
        inside { body = body $0 "\n" }
    ' "$page"
done

if [ -s "$out/stray" ]; then
    sed 's/^/FAIL /' "$out/stray"
    fail=$((fail + $(wc -l < "$out/stray")))
fi

for program in "$out"/*.sword; do
    [ -f "$program" ] || continue
    base=${program%.sword}
    where=$(basename "$base" | sed 's/_\([0-9]*\)$/.md:\1/')
    if grep -E '^[[:space:]]*import[[:space:]]+([A-Za-z_][A-Za-z0-9_]*[[:space:]]+)?"' "$program" | grep -vqE '"std/'; then
        if [ -e "$base.out" ] || [ -e "$base.exit" ]; then
            echo "FAIL $where: says what it prints, but imports a package of its own and cannot run"
            fail=$((fail + 1))
        fi
        skip=$((skip + 1))
        continue
    fi
    if ! "$shield" "$program" -o "$out/program" > "$out/log" 2>&1; then
        echo "FAIL $where"
        sed 's/^/     /' "$out/log" | head -10
        fail=$((fail + 1))
        continue
    fi
    if [ ! -e "$base.out" ] && [ ! -e "$base.exit" ]; then
        pass=$((pass + 1))
        continue
    fi

    want_exit=0
    [ -e "$base.exit" ] && want_exit=$(cat "$base.exit")
    # A program that never finishes would stop the whole check; ten seconds is
    # far more than any example needs.
    perl -e 'alarm 10; exec @ARGV' "$out/program" < /dev/null > "$out/stdout" 2> "$out/stderr"
    got_exit=$?
    # The marker's last line always ends in a newline; a program's need not.
    [ -n "$(tail -c 1 "$out/stdout")" ] && echo >> "$out/stdout"
    if [ "$got_exit" != "$want_exit" ]; then
        echo "FAIL $where: exits with $got_exit, the page says $want_exit"
        sed 's/^/     /' "$out/stderr" | head -5
        fail=$((fail + 1))
    elif [ -e "$base.out" ] && ! cmp -s "$base.out" "$out/stdout"; then
        echo "FAIL $where: prints something other than the page says"
        diff "$base.out" "$out/stdout" | sed 's/^/     /' | head -10
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
        ran=$((ran + 1))
    fi
done

echo "docs: $pass examples pass ($ran of them also run as the page says), $fail fail, $skip import a package of their own"
[ "$fail" = 0 ]
