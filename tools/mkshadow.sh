#!/usr/bin/env bash
# Create mods/customer/mod/shadow: the stock /etc/shadow with a new root password
# (SHA-256 crypt "$5$", same scheme as stock). The password is typed interactively
# and never stored; the file is git-ignored.
#
# Usage: tools/mkshadow.sh [work_dir]   (default: work)
set -euo pipefail
WORK="${1:-work}"
STOCK="$WORK/fs/rootfs/etc/shadow"
OUT="${OUT:-mods/customer/mod/shadow}"       # OUT=mods/release/shadow tools/mkshadow.sh = the generic password file of the public release
[ -r "$STOCK" ] || { echo "$STOCK missing (run unpack.py --extract-fs)"; exit 1; }
HASH="$(openssl passwd -5)"          # prompts twice, no echo
case "$HASH" in '$5$'*) ;; *) echo "hash failed"; exit 1;; esac
awk -F: -v OFS=: -v h="$HASH" '$1=="root"{$2=h} {print}' "$STOCK" > "$OUT"
chmod 600 "$OUT"
grep -c '^root:\$5\$' "$OUT" >/dev/null && echo "wrote $OUT (root password replaced, other lines stock)"
