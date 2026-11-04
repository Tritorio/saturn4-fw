#!/usr/bin/env bash
# Send one `tiltreg` command to the printer's libsgtap (idle or dry run) and print the answer; retries while the motor moves.
# Usage: TPW=<pw> tools/tiltreg.sh [HOST] "status|stock|stealth 0|1|pwm A G F S|chop TOFF TBL HSTRT HEND|irun N|wave P|lut L0..L7 SEL START|cur"
# Needs libsgtap with tiltreg (a286252b or later, loaded after a power cycle). See docs/tilt_motor.txt, docs/tilt_best_settings.txt.
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=192.168.0.44
[ $# -gt 1 ] && { HOST=$1; shift; }
: "${TPW:?set TPW to the printer password}"
ID=$(date +%H%M%S)
TPW=$TPW tools/tsh.py "$HOST" "r(){ echo \"\$2\" > /tmp/sgweb/cmd.\$1; sleep 1; grep -o '\"res\":\"[^\"]*\"' /tmp/sgweb/live.json; }; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do r ${ID}\$i \"tiltreg $1\"; grep -q moving /tmp/sgweb/live.json || break; done" 2>&1 | sed 1,/__END_/d | grep '"res"' | tail -1
