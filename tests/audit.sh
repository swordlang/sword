#!/bin/sh
# The suite with every program built under --audit, where each bounds check
# the compiler dropped stays in as one that panics with a message of its own.
# Every test passing as it does without means none of those checks was needed.
#
# shield is a wrapper here, which two tests cannot see through: the flags file
# test copies shield beside a flags file of its own, and the --audit test
# needs a build without it. Those two are expected to fail and nothing else.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/shield" <<EOF
#!/bin/sh
case "\${1:-}" in
    doc|clean) exec "$root/shield" "\$@" ;;
esac
exec "$root/shield" "\$@" --audit
EOF
chmod +x "$tmp/shield"
SHIELD="$tmp/shield" "$root/tests/run.sh" > "$tmp/out" 2>&1
tail -1 "$tmp/out"
if grep -q 'compiler dropped' "$tmp/out"; then
    echo "FAIL a dropped bounds check failed:"
    grep -B2 'compiler dropped' "$tmp/out"
    exit 1
fi
others=$(grep '^FAIL' "$tmp/out" | grep -v -e '^FAIL flags file of two lines' -e '^FAIL --audit:')
if [ -n "$others" ]; then
    printf '%s\n' "$others"
    exit 1
fi
echo "audit: no dropped check was needed"
