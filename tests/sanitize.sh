#!/bin/sh
# tests/sanitize.sh address|thread [test...]
#
# Builds the runtime again under a sanitizer, links each test program against
# that copy, runs it, and holds it to the exit status its first comment
# declares. `address` is AddressSanitizer with UndefinedBehaviorSanitizer and
# goes over every test that runs; `thread` is ThreadSanitizer over the tests
# where tasks meet. Name tests to run only those.
#
# The runtime tells both sanitizers about every stack switch, so a report here
# is about the program, not about fibers confusing the tool.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
shield=$root/shield
mode=${1:-}
[ $# -gt 0 ] && shift

case $mode in
address) flags="-fsanitize=address,undefined -fno-omit-frame-pointer"
         attribute=sanitize_address ;;
thread)  flags="-fsanitize=thread"
         attribute=sanitize_thread ;;
*) echo "usage: tests/sanitize.sh address|thread [test...]" >&2; exit 2 ;;
esac

if [ ! -x "$shield" ] || [ ! -f "$root/libsword_rt.flags" ]; then
    echo "build first: make" >&2
    exit 2
fi

# The concurrency tests: everything that spawns, waits, locks or serves.
concurrent="blocking parallel scope atomics shared http router httpmany deadline
testparallel files metrics channels chunked graceful signals libcnames tls https
filepool static clientreply lockpark dialreuse dialrefused lockread httpwritefail
lockreadmany externblocking boundsprovenmut parallelcontinue floatlanes
selectlock parallelinvariants parallelunwalked overflowlate parallelshort
parallelnested parallelpieces parallelpiecesread returnedlegal"

if [ $# -eq 0 ]; then
    if [ "$mode" = thread ]; then
        set -- $concurrent
    else
        set -- $(cd "$root/tests" && grep -l '^// expect: ' *.sword | sed 's/\.sword$//')
    fi
fi

out=$(mktemp -d "${TMPDIR:-/tmp}/sword-$mode.XXXXXX")
trap 'rm -rf "$out"' EXIT

# The link line the ordinary build wrote says whether TLS is in, and where
# OpenSSL lives when it is not in the system's own directories.
linkflags=$(cat "$root/libsword_rt.flags")
tls=""
case " $linkflags " in
*" -lssl "*)
    tls="-DSWORD_HAVE_TLS=1"
    for word in $linkflags; do
        case $word in -L*/lib) tls="$tls -I${word#-L}/../include" ;; esac
    done ;;
esac

# Where the C++ library is GCC's, clang uses the newest GCC it can find, which
# on Ubuntu 24.04 is a GCC 14 with no libstdc++ headers beside it. The GCC that
# c++ names is the one that has them.
if ${CXX:-c++} --version 2>/dev/null | grep -q "Free Software Foundation"; then
    flags="$flags --gcc-install-dir=$(dirname "$(${CXX:-c++} -print-libgcc-file-name)")"
fi

objs=""
for src in "$root"/rt/*.cpp; do
    o="$out/$(basename "$src" .cpp).o"
    clang++ -std=c++17 -O1 -g $flags $tls -c "$src" -o "$o" || exit 1
    objs="$objs $o"
done
clang -c "$root/rt/sword_ctx.S" -o "$out/sword_ctx.o" || exit 1
objs="$objs $out/sword_ctx.o"
# An archive, as the ordinary build links it: a program that never reaches
# std/tls takes no TLS object, and --emit-link names no OpenSSL for it.
rm -f "$out/libsword_rt.a"
ar rcs "$out/libsword_rt.a" $objs || exit 1
objs="$out/libsword_rt.a"

# A test that never finishes is reported like any other failure, instead of
# holding the whole run until something outside kills it.
cap() { perl -e 'alarm shift; exec @ARGV or exit 127' "$@"; }

pass=0
fail=0
for name in "$@"; do
    src="$root/tests/$name.sword"
    want=$(sed -n 's|^// expect: *||p' "$src" | head -1)
    if ! "$shield" "$src" --emit-llvm > "$out/$name.raw.ll" 2> "$out/$name.log"; then
        echo "FAIL $name: does not compile"
        sed 's/^/     /' "$out/$name.log" | head -5
        fail=$((fail + 1))
        continue
    fi
    # Starting from IR, clang instruments only the functions whose attributes
    # ask for it, and shield writes no such attribute: without this line the
    # sanitizer would see the runtime and none of the program's own code.
    sed "s/^attributes #\([0-9]*\) = { /attributes #\1 = { $attribute /" \
        "$out/$name.raw.ll" > "$out/$name.ll"
    # What shield itself would link against: the runtime's libraries, those the
    # test's extern declarations name, and its weak symbols.
    if ! clang++ -O1 -g $flags -Wno-override-module -x ir "$out/$name.ll" \
            -x none $objs $("$shield" "$src" --emit-link) -o "$out/$name" \
            2> "$out/$name.log"; then
        echo "FAIL $name: does not link"
        sed 's/^/     /' "$out/$name.log" | head -5
        fail=$((fail + 1))
        continue
    fi
    # On Linux, AddressSanitizer also looks for leaks when the program ends.
    # Under OpenSSL it reports memory inside the library although every
    # context and connection the tests make is freed, so a test that uses
    # TLS is checked for everything but that (issue #30).
    # (macOS has no leak checking, and refuses to be asked for it.)
    options=${ASAN_OPTIONS:-}
    grep -q '^import "std/tls"' "$src" && options="detect_leaks=0:$options"
    (cd "$out" && ASAN_OPTIONS=$options cap 300 "./$name" > "$name.out" 2> "$name.log")
    got=$?
    if [ "$got" != "$want" ] || grep -q "Sanitizer\|runtime error" "$out/$name.log"; then
        echo "FAIL $name: exit $got, want $want"
        grep -A12 "Sanitizer\|runtime error" "$out/$name.log" | head -30 | sed 's/^/     /'
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
done

echo "$mode: $pass passed, $fail failed"
[ "$fail" = 0 ]
