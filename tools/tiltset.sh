#!/usr/bin/env bash
# Live change of the tilt (X) profile of the running chitu app (8.16, normal mode only).
# NOTE: libsgtap now keeps the profile of the Tilt speed card (custom values / switch) and writes it back within 0.5 s: use the card, not this script.
# Usage: TPW=<pw> tools/tiltset.sh [HOST] NAME=VALUE ...      (no NAME=VALUE: read only)
#   NAME = A B C D E F (first N layers) or S P R T X Y (after N), letters as in I200 of the
#   machine settings file: A/S return stage 1 speed, B/P return stage 2 speed, C/R release
#   stage 1 speed, D/T release stage 2 speed (deg/s, 10-72), E/X return stage-2 angle,
#   F/Y release stage-1 angle (deg, 0-140, 0 = single stage).
# Writes the floats at 0x39fb40 (live machine params, normal-mode rotate_param) through
# /proc/<chitu>/mem, only during the wait after retract (print state 2, waits up to 60 s),
# then reads the block back. Live only: not saved to machine_param_ref.bin (the app may save
# it later with its own saver). Refuses stage angles > 0 unless sgtilt (sglog/TILT2) is
# installed: stock firmware mishandles them.
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=192.168.0.44
[ $# -gt 0 ] && [[ "$1" != *=* ]] && { HOST=$1; shift; }
: "${TPW:?set TPW to the printer password}"
declare -A OFF=([A]=0 [B]=4 [C]=8 [D]=12 [E]=16 [F]=20 [S]=24 [P]=28 [R]=32 [T]=36 [X]=40 [Y]=44)
writes="" angles=0
for kv in "$@"; do
    k=${kv%%=*} v=${kv#*=}
    [ -n "${OFF[$k]+x}" ] || { echo "unknown name: $k (A-F, S P R T X Y)"; exit 2; }
    [[ "$v" =~ ^[0-9]+(\.[0-9]+)?$ ]] || { echo "$k: not a number: $v"; exit 2; }
    case $k in
        E|F|X|Y) python3 -c "import sys; sys.exit(not 0 <= $v <= 140)" || { echo "$k: angle 0-140"; exit 2; }
                 python3 -c "import sys; sys.exit(not $v > 0)" && angles=1 ;;
        *)       python3 -c "import sys; sys.exit(not 10 <= $v <= 72)" || { echo "$k: speed 10-72"; exit 2; } ;;
    esac
    oct=$(python3 -c "import struct; print(''.join('\\\\%03o' % b for b in struct.pack('<f', $v)))")
    writes+="w ${OFF[$k]} '$oct'; "
done
remote='M=; for p in $(pidof chitu); do grep -q libsgtap /proc/$p/maps && M=/proc/$p/mem; done
[ -n "$M" ] || { echo "chitu with libsgtap not found"; exit 1; }
rd(){ dd if=$M bs=1 skip=$1 count=$2 2>/dev/null | od -A n -t x1 | tr -d " \n"; }
w(){ printf "$2" | dd of=$M bs=1 seek=$((0x39fb40 + $1)) conv=notrunc 2>/dev/null; }'
if [ -n "$writes" ]; then
    [ $angles = 1 ] && remote+='
grep -q "\"inst\":1" /tmp/sgweb/live.json || { echo "REFUSED: sgtilt not installed"; exit 1; }'
    remote+='
PL=$(rd $((0x2eb6d8)) 4); P=$((0x${PL:6:2}${PL:4:2}${PL:2:2}${PL:0:2})); s=
[ $P -ne 0 ] && for i in $(seq 600); do s=$(rd $((P + 0x30b)) 1); [ "$s" = 02 ] && break; usleep 100000; done
if [ "$s" = 02 ]; then '"$writes"' echo WRITTEN in the wait; else echo "NOT WRITTEN: no wait in 60 s"; fi'
fi
remote+='
dd if=$M bs=4 skip=$((0x39fb40 / 4)) count=12 2>/dev/null | od -A n -t f4 -v'
remote="( ${remote//$'\n'/; } )"       # one command line; exit only leaves the subshell
[ -n "${TILTSET_DRY:-}" ] && { echo "$remote"; echo "length ${#remote}"; exit 0; }
out=$(TTIMEOUT=150 tools/tsh.py "$HOST" "$remote" 2>&1 | sed '1,/__END_/d')   # drop the echo
vals=$(echo "$out" | grep -E '^ +-?[0-9]' | tr -s ' \n' ' ' || true)
echo "$out" | grep -E 'WRITTEN|REFUSED|not found' || true
[ -n "${TILTSET_DEBUG:-}" ] && echo "$out"
python3 - "$vals" <<'E'
import sys
v = [float(x) for x in sys.argv[1].split()]
if len(v) == 12:
    n = 'ABCDEFSPRTXY'
    print('first N: ' + ' '.join(f'{n[i]}{v[i]:g}' for i in range(6)))
    print('after N: ' + ' '.join(f'{n[i]}{v[i]:g}' for i in range(6, 12)))
else:
    print('read-back failed')
E
