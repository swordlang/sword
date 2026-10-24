#!/bin/sh
# tests/docs.sh — every example program in the documentation compiles.
#
# A ```sword block with a `func main` in it is a whole program, and a reader
# will paste it; it has to build as written. A block that imports a package of
# the reader's own, rather than one from std, is showing how packages fit
# together and cannot be built on its own, so it is left out.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
shield=$root/shield
out=$(mktemp -d "${TMPDIR:-/tmp}/sword-docs.XXXXXX")
trap 'rm -rf "$out"' EXIT

pass=0
fail=0
skip=0
for page in "$root"/docs/*.md "$root"/README.md "$root"/CONTRIBUTING.md \
        "$root"/editors/README.md "$root"/editors/vscode/README.md; do
    [ -f "$page" ] || continue
    # One file per block, named for the page and the line the block starts on.
    awk -v dir="$out" -v page="$(basename "$page" .md)" '
        /^```sword[ \t]*$/ { inside = 1; start = NR; body = ""; next }
        /^```/ && inside {
            inside = 0
            if (body ~ /func main\(/) {
                file = dir "/" page "_" start ".sword"
                printf "%s", body > file
                close(file)
            }
            next
        }
        inside { body = body $0 "\n" }
    ' "$page"
done

for program in "$out"/*.sword; do
    [ -f "$program" ] || continue
    where=$(basename "$program" .sword | sed 's/_\([0-9]*\)$/.md:\1/')
    if grep -E '^[[:space:]]*import[[:space:]]+([A-Za-z_][A-Za-z0-9_]*[[:space:]]+)?"' "$program" | grep -vqE '"std/'; then
        skip=$((skip + 1))
        continue
    fi
    if "$shield" "$program" -o "$out/program" > "$out/log" 2>&1; then
        pass=$((pass + 1))
    else
        echo "FAIL $where"
        sed 's/^/     /' "$out/log" | head -10
        fail=$((fail + 1))
    fi
done

echo "docs: $pass examples compile, $fail do not, $skip import a package of their own"
[ "$fail" = 0 ]
