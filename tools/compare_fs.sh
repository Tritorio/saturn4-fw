#!/usr/bin/env bash
# Compare two extracted UBIFS trees: file content + metadata (type, mode, owner, symlink
# target, size, mtime). Ownership is read through each tree's fakeroot state.
#
# Usage: tools/compare_fs.sh DIR_A STATE_A DIR_B STATE_B
set -euo pipefail

[ $# -eq 4 ] || { sed -n '2,6p' "$0"; exit 2; }
A="$1"; SA="$2"; B="$3"; SB="$4"

listing() {  # dir state
    (cd "$1" && fakeroot -i "$2" find . -printf '%y %m %U:%G %s %T@ %p -> %l\n' | sort -k6)
}

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
listing "$A" "$(realpath "$SA")" > "$TMP/a"
listing "$B" "$(realpath "$SB")" > "$TMP/b"
# directory sizes depend on the host filesystem, drop them
sed -E -i 's/^(d [0-7]+ [0-9:]+) [0-9]+/\1 -/' "$TMP/a" "$TMP/b"

rc=0
if ! diff -u "$TMP/a" "$TMP/b" > "$TMP/meta.diff"; then
    echo "METADATA DIFFERS:"; head -40 "$TMP/meta.diff"; rc=1
else
    echo "metadata identical ($(wc -l < "$TMP/a") entries)"
fi
if ! diff -r --no-dereference "$A" "$B" > "$TMP/content.diff"; then
    echo "CONTENT DIFFERS:"; head -40 "$TMP/content.diff"; rc=1
else
    echo "content identical"
fi
exit $rc
