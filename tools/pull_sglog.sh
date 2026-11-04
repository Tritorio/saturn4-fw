#!/usr/bin/env bash
# Pull the strain-gauge logs from the printer over telnet (read-only on the printer).
# Usage: tools/pull_sglog.sh [-c] [HOST]     (HOST default 192.168.0.44)
#   -c  also pull the raw serial captures (sgcap/, a few MB, slower)
# Password: env TPW, else asked (not echoed, never stored).
# Result: backups/sglog/pull_<YYYYmmdd_HHMMSS>.tgz + .md5, extracted into backups/sglog/
#   (sglog/*.csv, sgcap/*.bin), every file md5-checked against the printer.
#   Files that change during the pull (log of a running/just stopped print, the open raw
#   capture) get md5s before and after the tar on the printer: those that differ are
#   reported as "still being written" (copy may be incomplete), not as an error.
set -euo pipefail
cd "$(dirname "$0")/.."
DIRS=sglog
[ "${1:-}" = -c ] && { DIRS="sglog sgcap"; shift; }
HOST=${1:-192.168.0.44}
if [ -z "${TPW:-}" ]; then
    read -rsp "root password for $HOST: " TPW; echo
fi
export TPW TTIMEOUT=${TTIMEOUT:-900}

TS=$(date +%Y%m%d_%H%M%S)
OUT=backups/sglog; mkdir -p "$OUT" build
RAW=build/pull_$TS.out TGZ=$OUT/pull_$TS.tgz SUM=$OUT/pull_$TS.md5

echo "pulling $DIRS from $HOST ..."
# one line only: a newline makes the shell print a prompt in front of the marker
GLOB=$(for d in $DIRS; do printf '%s/* ' $d; done)
tools/tsh.py "$HOST" "cd /media/mmcblk0p2 && md5sum $GLOB> /tmp/sgpull.md5; echo ==B\"\"EGIN==; tar cf - $DIRS | gzip | base64; echo ==E\"\"ND==; cat /tmp/sgpull.md5; echo ==A\"\"FTER==; md5sum $GLOB; rm /tmp/sgpull.md5" > "$RAW"

tr -d '\r' < "$RAW" | sed -n '/==BEGIN==$/,/^==END==$/{//!p}' | base64 -d > "$TGZ" ||
    { echo "decode failed, raw output kept in $RAW" >&2; exit 1; }
tr -d '\r' < "$RAW" | sed -n '/^==END==$/,/^==AFTER==$/p' | grep -E '^[0-9a-f]{32}  ' > "$SUM" ||
    { echo "no md5 list in the output, raw output kept in $RAW" >&2; exit 1; }
AFTER=$(tr -d '\r' < "$RAW" | sed -n '/^==AFTER==$/,$p' | grep -E '^[0-9a-f]{32}  ' || true)
tar xzf "$TGZ" -C "$OUT"
BAD=0 BUSY=0
while read -r sum name; do
    [ "$(cd "$OUT" && md5sum "$name" | cut -d' ' -f1)" = "$sum" ] && continue
    if ! grep -qxF "$sum  $name" <<< "$AFTER"; then
        echo "still being written: $name (copy may be incomplete, pull again later)" >&2
        BUSY=$((BUSY + 1))
    else
        echo "md5 MISMATCH: $name" >&2
        BAD=$((BAD + 1))
    fi
done < "$SUM"
[ $BAD -eq 0 ] || { echo "$BAD file(s) corrupted, raw output kept in $RAW" >&2; exit 1; }
rm -f "$RAW"

W=""; [ $BUSY -eq 0 ] || W=", $BUSY still being written"
echo "OK: $(($(wc -l < "$SUM") - BUSY)) files verified$W -> $TGZ"
ls -lt "$OUT/sglog/"*.csv | head -5
