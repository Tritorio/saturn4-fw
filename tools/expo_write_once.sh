#!/bin/sh
# ONE-SHOT test (user OK ): in the next wait after retract (P+0x30b == 2) write the exposure time P+0x24c = $1 ms (float) for that layer only.
# Usage on the printer: sh expo_write_once.sh 5000   (log: /tmp/expo_write.txt). Refuses outside a wait, stops after one write or after 20 min.
V=${1:?ms}; OUT=/tmp/expo_write.txt
M=; for p in $(pidof chitu); do M=/proc/$p/mem; done
[ -n "$M" ] || { echo "chitu not found" > $OUT; exit 1; }
rd(){ dd if=$M bs=4 skip=$(($1 / 4)) count=$2 2>/dev/null | od -A n -t x4 -v | tr -d '\n'; }
oct=$(awk -v v="$V" 'BEGIN{}' 2>/dev/null)
case $V in 5000) OCT='\000\100\234\105';; *) echo "only 5000 is coded" > $OUT; exit 1;; esac
i=0
while [ $i -lt 12000 ]; do
  P=$((0x$(echo $(rd $((0x2eb6d8)) 1))))
  if [ $P -ne 0 ]; then
    w=$((0x$(echo $(rd $((P + 0x308)) 1)))); s=$(( (w >> 24) & 255 ))
    L=$((0x$(echo $(rd $((P + 0x2ac)) 1))))
    if [ $s -eq 2 ] && [ $L -ge 3 ]; then
      before=$(rd $((P + 0x24c)) 1)
      printf "$OCT" | dd of=$M bs=1 seek=$((P + 0x24c)) conv=notrunc 2>/dev/null
      after=$(rd $((P + 0x24c)) 1)
      echo "WRITTEN layer=$L uptime=$(cut -d' ' -f1 /proc/uptime) before=$before after=$after" > $OUT
      exit 0
    fi
  fi
  usleep 100000; i=$((i+1))
done
echo "NOT WRITTEN (timeout)" > $OUT
