#!/bin/sh
# The suite, and then generated programs, with every program built under
# --audit=footprints: each task and each piece of a parallel loop is granted
# what its arguments reach, and two live ones reaching one byte, one of them
# to write, abort the program with both places. The race checker promises
# that never happens in a program it accepts; this checks the promise on
# every run, on one thread as on many, where a race itself would show only
# in some schedule.
#
#   tests/audit-footprints.sh [first [last]]   generator seeds, default 1001..1400
#
# shield is a wrapper here, which two tests cannot see through: the flags file
# test copies shield beside a flags file of its own, and the --audit test
# needs a build without it. Those two are expected to fail and nothing else.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
first=${1:-1001}
last=${2:-1400}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/shield" <<WRAP
#!/bin/sh
case "\${1:-}" in
    doc|clean) exec "$root/shield" "\$@" ;;
esac
exec "$root/shield" "\$@" --audit=footprints
WRAP
chmod +x "$tmp/shield"
SHIELD="$tmp/shield" "$root/tests/run.sh" > "$tmp/out" 2>&1
tail -1 "$tmp/out"
status=0
if grep -q 'footprint overlap' "$tmp/out"; then
    echo "FAIL two live tasks reached one byte, one writing:"
    grep -B2 'footprint overlap' "$tmp/out"
    status=1
fi
others=$(grep '^FAIL' "$tmp/out" | grep -v -e '^FAIL flags file of two lines' -e '^FAIL --audit:')
if [ -n "$others" ]; then
    printf '%s\n' "$others"
    status=1
fi

# Generated programs the checker accepts, from seeds that fixed nothing.
accepted=0
for s in $(seq "$first" "$last"); do
    python3 -I "$root/tests/racegen/gen.py" "$s" "$tmp/g.sword" > /dev/null
    "$root/shield" --audit=footprints "$tmp/g.sword" -o "$tmp/g.bin" \
        > /dev/null 2>&1 || continue
    accepted=$((accepted + 1))
    for t in 1 4; do
        if ! SWORD_THREADS=$t perl -e 'alarm 10; exec @ARGV' "$tmp/g.bin" \
            > /dev/null 2> "$tmp/g.err"; then
            echo "FAIL seed $s at $t threads:"
            cat "$tmp/g.err"
            status=1
        fi
    done
done
echo "generated: $accepted of seeds $first..$last accepted, each run at 1 and 4 threads"
[ "$status" -eq 0 ] && echo "audit: no two live tasks reached one byte with one writing"
exit "$status"
