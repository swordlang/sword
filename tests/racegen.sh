#!/bin/sh
# tests/racegen.sh [first [last]]
#
# Generated programs against the race checker. tests/racegen/gen.py writes a
# program for each seed and knows whether it races; the compiler is asked,
# and every program it accepts runs under forty simulated schedules and at
# four pool sizes. A racy program that compiles is a hole in the checker,
# and a program that compiles and gives two answers is one too, whatever the
# generator thought of it. Both make the run fail.
#
# Refusing a program that does not race is not a failure: the checker is
# cautious on purpose. Those are counted, and the counts are the price of
# the caution.
#
# Not part of make test or CI: the four hundred seeds it runs by default
# took 35 seconds on an M4.
#
#   SWORD_RACEGEN_OUT  keep each seed's program and output here (default: discarded)
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
shield="$root/shield"
first=${1:-1}
last=${2:-400}

if [ ! -x "$shield" ]; then
    echo "build first: make" >&2
    exit 2
fi
if [ -n "${SWORD_RACEGEN_OUT:-}" ]; then
    out=$SWORD_RACEGEN_OUT
    mkdir -p "$out"
else
    out=$(mktemp -d)
    trap 'rm -rf "$out"' EXIT
fi

run() {
    perl -e 'alarm 10; exec @ARGV' "$out/$1.bin" >> "$out/$1.out" 2>&1 ||
        echo "exit $?" >> "$out/$1.out"
}

holes=0 racy_refused=0 free_refused=0 free_accepted=0
for s in $(seq "$first" "$last"); do
    racy=$(python3 -I "$root/tests/racegen/gen.py" "$s" "$out/$s.sword" |
        sed 's/.*"racy": \([a-z]*\).*/\1/')
    if ! "$shield" "$out/$s.sword" -o "$out/$s.bin" > "$out/$s.log" 2>&1; then
        if grep -q "error:" "$out/$s.log"; then
            [ "$racy" = true ] && racy_refused=$((racy_refused + 1)) ||
                free_refused=$((free_refused + 1))
        else
            echo "seed $s: the compiler failed without a diagnostic"
            holes=$((holes + 1))
        fi
        continue
    fi
    : > "$out/$s.out"
    for k in $(seq 1 40); do SWORD_SIM_SEED=$k run "$s"; done
    for t in 1 2 4 8; do SWORD_THREADS=$t run "$s"; done
    answers=$(sort -u "$out/$s.out" | wc -l | tr -d ' ')
    if [ "$racy" = true ]; then
        echo "seed $s: racy and accepted, $answers answers"
        holes=$((holes + 1))
    elif [ "$answers" -ne 1 ]; then
        echo "seed $s: accepted with $answers answers"
        holes=$((holes + 1))
    else
        free_accepted=$((free_accepted + 1))
    fi
done

echo "seeds $first..$last: $holes accepted wrongly, $racy_refused racy refused," \
    "$free_accepted race-free accepted, $free_refused race-free refused"
[ "$holes" -eq 0 ]
