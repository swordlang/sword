#!/bin/sh
# Modules that come from somewhere else: fetching what sword.mod requires,
# choosing versions, sword.sum, shield get and shield vendor, and shield new. The repositories
# are local, and git's own url.<base>.insteadOf points https:// at them, so
# nothing here touches the network and the compiler has no hook for tests.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
shield="$root/shield"
tmp=$(mktemp -d)
# The cache is read-only on purpose.
trap 'chmod -R u+w "$tmp" 2>/dev/null; rm -rf "$tmp"' EXIT

SWORD_CACHE="$tmp/cache"
GIT_CONFIG_COUNT=1
GIT_CONFIG_KEY_0="url.file://$tmp/repos/.insteadOf"
GIT_CONFIG_VALUE_0=https://
export SWORD_CACHE GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0

pass=0
fail=0

g() {
    git -c user.name=test -c user.email=test@example.com \
        -c init.defaultBranch=main -c commit.gpgsign=false -c tag.gpgsign=false "$@"
}

# repo <module path> <file> <text>...: a repository with those files, committed.
repo() {
    dir="$tmp/repos/$1.git"
    shift
    mkdir -p "$dir"
    [ -d "$dir/.git" ] || g -C "$dir" init -q
    while [ $# -ge 2 ]; do
        printf '%s' "$2" > "$dir/$1"
        shift 2
    done
    g -C "$dir" add -A
    g -C "$dir" commit -qm change
}

tag() { g -C "$tmp/repos/$1.git" tag -f "$2" >/dev/null; }

func() { # name, value
    printf 'func %s() int {\n    return %s\n}\n' "$1" "$2"
}

ok() { pass=$((pass + 1)); }
bad() {
    echo "FAIL $1"
    [ -s "$tmp/out" ] && sed 's/^/     /' "$tmp/out"
    fail=$((fail + 1))
}

# expect_exit <name> <exit status> [shield args]: in the site, keeping what it
# said.
expect_exit() {
    name=$1 want=$2
    shift 2
    (cd "$tmp/site" && "$shield" "$@") >"$tmp/out" 2>&1
    got=$?
    if [ "$got" = "$want" ]; then ok; else bad "$name: exit $got, want $want"; fi
}

expect_said() { # name, text, shield args
    name=$1 want=$2
    shift 2
    (cd "$tmp/site" && "$shield" "$@") >"$tmp/out" 2>&1
    if [ $? -ne 0 ] && grep -qF -- "$want" "$tmp/out"; then ok; else bad "$name: want '$want'"; fi
}

# The program's answer, built with whatever is around it.
answer() { # name, want, extra shield args
    name=$1 want=$2
    shift 2
    if (cd "$tmp/site" && "$shield" . -o "$tmp/prog" "$@") >"$tmp/out" 2>&1; then
        "$tmp/prog"
        got=$?
        if [ "$got" = "$want" ]; then ok; else bad "$name: answered $got, want $want"; fi
    else
        bad "$name: did not build"
    fi
}

# lib has v1.0.0, v1.1.0, a pre-release, and a commit after them all.
repo example.com/lib sword.mod "module example.com/lib
sword 0.2
" lib.sword "package lib

$(func Thirty 30)" LICENSE "MIT" notes.txt "not code"
tag example.com/lib v1.0.0
repo example.com/lib lib.sword "package lib

$(func Thirty 30)
$(func Forty 40)"
tag example.com/lib v1.1.0
tag example.com/lib v1.2.0-rc.1
repo example.com/lib lib.sword "package lib

$(func Thirty 30)
$(func Forty 40)
$(func Fifty 50)"

# helper needs a newer lib than the site asks for.
repo example.com/helper sword.mod "module example.com/helper
sword 0.2

require example.com/lib v1.1.0
" helper.sword "package helper

import \"example.com/lib\"

$(func Two 'lib.Forty() - 38')"
tag example.com/helper v0.1.0

# Two majors of one module, in one repository.
repo example.com/deep sword.mod "module example.com/deep
sword 0.2
" deep.sword "package deep

$(func N 1)"
tag example.com/deep v1.0.0
repo example.com/deep sword.mod "module example.com/deep/v2
sword 0.2
" deep.sword "package deep

$(func N 2)"
tag example.com/deep v2.0.0

# One that imports what it never required.
repo example.com/sneaky sword.mod "module example.com/sneaky
sword 0.2
" sneaky.sword "package sneaky

import \"example.com/lib\"

$(func N 'lib.Thirty()')"
tag example.com/sneaky v0.1.0

mkdir -p "$tmp/site"
cat > "$tmp/site/sword.mod" <<'EOF'
// The site.
module example.com/site
sword 0.2
EOF
cat > "$tmp/site/main.sword" <<'EOF'
import "example.com/lib"
import "example.com/helper"

func main() int {
    return lib.Thirty() + helper.Two()
}
EOF

# An import nobody required says how to require it.
expect_said "unrequired import" "'shield get <module>' adds the module" .

# get with a version writes it, and the comment above stays.
expect_exit "get lib@v1.0.0" 0 get example.com/lib@v1.0.0
if grep -qx 'require example.com/lib v1.0.0' "$tmp/site/sword.mod" &&
    grep -qx '// The site.' "$tmp/site/sword.mod"; then ok; else bad "get wrote $(cat "$tmp/site/sword.mod")"; fi

# get with no version takes the newest release, not the pre-release or the
# untagged commit, and the build takes the newest lib anybody required.
expect_exit "get helper" 0 get example.com/helper
grep -qx 'require example.com/helper v0.1.0' "$tmp/site/sword.mod" || bad "get helper wrote the wrong version"
answer "minimal version selection" 32

# Every version read is in sword.sum, in order.
if [ "$(cut -d' ' -f1-2 "$tmp/site/sword.sum" | tr '\n' ,)" = \
    "example.com/helper v0.1.0,example.com/lib v1.0.0,example.com/lib v1.1.0," ]; then ok
else bad "sword.sum: $(cat "$tmp/site/sword.sum")"; fi

# The hash is of the files the compiler reads, and anyone can work it out.
if command -v sha256sum >/dev/null; then sum256() { sha256sum "$@"; }
else sum256() { shasum -a 256 "$@"; }; fi
byhand=$(cd "$tmp/cache/mod/example.com/lib@v1.1.0" &&
    find . -name '*.sword' -o -name sword.mod | sed 's|^\./||' | LC_ALL=C sort |
    while read -r f; do sum256 "$f"; done | sum256 | cut -d' ' -f1)
if grep -q "example.com/lib v1.1.0 sha256:$byhand" "$tmp/site/sword.sum"; then ok
else bad "the hash of lib v1.1.0 is not $byhand"; fi

# The cache keeps code and licence only, and nobody writes to it.
if [ -f "$tmp/cache/mod/example.com/lib@v1.1.0/LICENSE" ] &&
    [ ! -e "$tmp/cache/mod/example.com/lib@v1.1.0/notes.txt" ] &&
    [ ! -w "$tmp/cache/mod/example.com/lib@v1.1.0/lib.sword" ]; then ok
else bad "the cache holds $(ls -l "$tmp/cache/mod/example.com/lib@v1.1.0")"; fi

# Without a version, get does not go back from a pre-release.
expect_exit "get lib@v1.2.0-rc.1" 0 get example.com/lib@v1.2.0-rc.1
expect_exit "get lib" 0 get example.com/lib
grep -qx 'require example.com/lib v1.2.0-rc.1' "$tmp/site/sword.mod" || bad "get went back from the pre-release"

# A commit with no tag is a pseudo-version, which sorts before every release.
expect_exit "get lib@main" 0 get example.com/lib@main
day=$(TZ=UTC0 g -C "$tmp/repos/example.com/lib.git" log -1 --format=%cd --date=format-local:%Y%m%d)
head=$(g -C "$tmp/repos/example.com/lib.git" rev-parse HEAD | cut -c1-12)
grep -qx "require example.com/lib v0.0.0-$day-$head" "$tmp/site/sword.mod" || bad "pseudo-version: $(grep lib "$tmp/site/sword.mod")"
if grep -q 'a build uses v1.1.0' "$tmp/out"; then ok; else bad "get did not say helper's lib wins"; fi
answer "pseudo-version below a release" 32

expect_said "a revision that is not there" "'nosuch' is not a version, a branch or a commit" get example.com/lib@nosuch
expect_said "v2 under a v1 path" "required as 'example.com/lib/v2'" get example.com/lib@v2.0.0
expect_said "a repository that is not there" "cannot list the versions of example.com/nothing" get example.com/nothing
expect_said "a module's own package" "is part of this module" get example.com/site/util

# A module required only through another cannot be imported directly.
expect_exit "get lib@none" 0 get example.com/lib@none
if ! grep -q 'example.com/lib' "$tmp/site/sword.mod" && grep -q 'example.com/lib v1.1.0' "$tmp/site/sword.sum" &&
    ! grep -q 'example.com/lib v0.0.0' "$tmp/site/sword.sum"; then ok
else bad "@none left $(cat "$tmp/site/sword.mod" "$tmp/site/sword.sum")"; fi
expect_said "import through another" "'example.com/lib' is required only by another module" .
expect_exit "get lib back" 0 get example.com/lib@v1.0.0

# --locked refuses to add to sword.sum; without it the line comes back.
cp "$tmp/site/sword.sum" "$tmp/sum"
grep -v helper "$tmp/sum" > "$tmp/site/sword.sum"
expect_said "--locked" "is not in sword.sum, and --locked keeps it from being added" . -o "$tmp/prog" --locked
answer "a missing line is added" 32
cmp -s "$tmp/sum" "$tmp/site/sword.sum" || bad "sword.sum came back different"

echo "nonsense" >> "$tmp/site/sword.sum"
expect_said "a line sword.sum cannot read" "expected '<path> <version> sha256:<hash>'" . -o "$tmp/prog"
cp "$tmp/sum" "$tmp/site/sword.sum"

# A tag moved to other code is caught by a cache that has not seen it yet.
repo example.com/helper helper.sword "package helper

import \"example.com/lib\"

$(func Two 'lib.Forty() - 37')"
tag example.com/helper v0.1.0
SWORD_CACHE="$tmp/cache2"
expect_said "moved tag" "example.com/helper v0.1.0 is not the code sword.sum recorded for it" . -o "$tmp/prog"
SWORD_CACHE="$tmp/cache"

# A dependency imports only what its own sword.mod requires.
expect_exit "get sneaky" 0 get example.com/sneaky
cat > "$tmp/site/main.sword" <<'EOF'
import "example.com/sneaky"

func main() int {
    return sneaky.N()
}
EOF
expect_said "a dependency's unrequired import" "example.com/sneaky imports 'example.com/lib', and its sword.mod does not require it" . -o "$tmp/prog"
expect_exit "get sneaky@none" 0 get example.com/sneaky@none

# Two majors are two modules, and imported by their own paths.
expect_exit "get deep/v2" 0 get example.com/deep/v2
cat > "$tmp/site/main.sword" <<'EOF'
import "example.com/deep/v2"

func main() int {
    return deep.N()
}
EOF
answer "deep/v2" 2

cat > "$tmp/site/main.sword" <<'EOF'
import "example.com/lib"
import "example.com/helper"

func main() int {
    return lib.Thirty() + helper.Two()
}
EOF

# shield doc reads a dependency from the cache, the way a build would find it.
said=$(cd "$tmp/site" && "$shield" doc example.com/lib 2>&1)
if printf '%s\n' "$said" | grep -qx 'func Forty() int' &&
    printf '%s\n' "$said" | grep -qx 'package lib // import "example.com/lib"'; then ok
else bad "shield doc example.com/lib: $said"; fi

# vendor/ is what the build reads, with no network and no cache.
expect_exit "vendor" 0 vendor
if [ "$(grep -v '^#' "$tmp/site/vendor/modules.txt" | tr '\n' ,)" = \
    "example.com/deep/v2 v2.0.0,example.com/helper v0.1.0,example.com/lib v1.1.0," ] &&
    [ -f "$tmp/site/vendor/example.com/lib/LICENSE" ]; then ok
else bad "vendor/modules.txt: $(cat "$tmp/site/vendor/modules.txt")"; fi
GIT_CONFIG_COUNT=0 SWORD_CACHE="$tmp/nowhere"
answer "built from vendor" 32
GIT_CONFIG_COUNT=1 SWORD_CACHE="$tmp/cache"
[ ! -e "$tmp/nowhere" ] || bad "a vendored build touched the cache"

# vendor/ sits in the project, so it is checked on every build.
chmod u+w "$tmp/site/vendor/example.com/lib/lib.sword"
echo '// edited' >> "$tmp/site/vendor/example.com/lib/lib.sword"
expect_said "edited vendor" "example.com/lib v1.1.0 is not the code sword.sum recorded for it" . -o "$tmp/prog"

# Asking for more than vendor/ has is caught before anything is built from it.
expect_exit "vendor again" 0 vendor
sed 's|require example.com/lib v1.0.0|require example.com/lib v1.2.0-rc.1|' "$tmp/site/sword.mod" > "$tmp/mod" &&
    cp "$tmp/mod" "$tmp/site/sword.mod"
expect_said "stale vendor" "run 'shield vendor' after changing sword.mod" . -o "$tmp/prog"

mkdir -p "$tmp/other/vendor"
printf 'module example.com/other\n' > "$tmp/other/sword.mod"
(cd "$tmp/other" && "$shield" vendor) >"$tmp/out" 2>&1
if [ $? -ne 0 ] && grep -qF "was not written by shield vendor" "$tmp/out"; then ok; else bad "vendor over somebody's vendor/"; fi

# shield new: a program and a library, each passing its own test.
mkdir -p "$tmp/new"
if (cd "$tmp/new" && "$shield" new example.com/hello && cd hello && "$shield" test . &&
        "$shield" . -o "$tmp/hello" && [ "$("$tmp/hello")" = "hello, world" ] &&
        grep -qx 'module example.com/hello' sword.mod) >"$tmp/out" 2>&1; then ok
else bad "shield new example.com/hello"; fi
if (cd "$tmp/new" && "$shield" new --lib example.com/sword-term term && cd sword-term &&
        "$shield" test . && grep -qx 'package term' term.sword) >"$tmp/out" 2>&1; then ok
else bad "shield new --lib example.com/sword-term term"; fi
(cd "$tmp/new" && "$shield" new --lib example.com/sword-cell) >"$tmp/out" 2>&1
if [ $? -ne 0 ] && grep -qF "'sword-cell' cannot be written in a program" "$tmp/out" &&
    [ ! -e "$tmp/new/sword-cell" ]; then ok; else bad "shield new --lib with no name to give"; fi
(cd "$tmp/new" && "$shield" new example.com/hello) >"$tmp/out" 2>&1
if [ $? -ne 0 ] && grep -qF "'hello' is already there, and not empty" "$tmp/out"; then ok
else bad "shield new over a directory with something in it"; fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
