#!/bin/sh
# machwatch.sh - started by hook.sh at once after the app (after the reset incident of 11-06).
# 1. For 150 s it watches the NEW part of the app log for "can't find machine parameters" / "can't find machine info": the app then
#    wrote DEFAULT machine settings over the saved ones. The newest valid backup is put back over the file (the running app keeps
#    the defaults in memory: ONLY A POWER CYCLE loads the restored file) and the event is logged to sglog/machguard.log and
#    sglog/MACHRESET (time stamp, remove it when you have seen it).
# 2. Then, every 60 s, a changed and valid settings file is saved as a rolling backup (machguard.tcl snap).
# Kill switch: /customer/mod/machguard_disable. Env (tests): MW_LOG (app log), MW_STATE (state dir), MW_WAIT (seconds), MW_STEP.
M=${MOD_DIR:-/customer/mod}
[ -f $M/machguard_disable ] && exit 0
LOG=${MW_LOG:-/media/mmcblk0p2/log1}
STATE=${MW_STATE:-/media/mmcblk0p2/sglog}
WAIT=${MW_WAIT:-150}
STEP=${MW_STEP:-60}
off=$(wc -c < "$LOG" 2>/dev/null || echo 0)
t=0
while [ $t -lt $WAIT ]; do
    sleep 3
    t=$((t + 3))
    sz=$(wc -c < "$LOG" 2>/dev/null || echo 0)
    [ "$sz" -lt "$off" ] && off=0          # the log was rotated
    if tail -c +$((off + 1)) "$LOG" 2>/dev/null | grep -a -q -E "can't find machine (parameters|info)"; then
        echo "$(date '+%F %T') the app reported missing machine parameters at start: saved settings restored, POWER CYCLE needed" >> $STATE/MACHRESET
        MACHGUARD_MODE=restore jimsh $M/machguard.tcl restore info
        break
    fi
done
while :; do
    sleep $STEP
    jimsh $M/machguard.tcl snap
done
