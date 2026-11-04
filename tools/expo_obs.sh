#!/bin/sh
# Read-only observer of the exposure variables of the running chitu app (docs/exposure_edit.txt). Runs ON the printer (busybox sh),
# started by tools/expo_obs_start.sh, writes a line to $OUT whenever one of the watched words changes. Never writes to app memory.
# words: layer(P+0x2ac) state(P+0x30b,0x30c) expo(P+0x24c) lightoff(0x250) rest_b(0x254) rest_a(0x258) wait(0x25c) t_wait(0x318/0x31c) t_exp(0x338/0x33c)
#        cur p_time + counters (0x2fda88+0x20,0x24,0x28)  bottom/trans counts (P+0x110, 0x114)
OUT=${OUT:-/tmp/expo_obs.txt}
M=; for p in $(pidof chitu); do M=/proc/$p/mem; done
[ -n "$M" ] || { echo "chitu not found"; exit 1; }
rd(){ dd if=$M bs=4 skip=$(($1 / 4)) count=$2 2>/dev/null | od -A n -t x4 -v | tr -d '\n'; }
last=; cnt=0
echo "# start $(cat /proc/uptime)" > $OUT
while [ ! -e /tmp/expo_obs_stop ] && [ $cnt -lt 2000000 ]; do
  P=$(rd $((0x2eb6d8)) 1); P=$((0x$(echo $P)))
  if [ $P -ne 0 ]; then
    a=$(rd $((P + 0x240)) 8)       # 0x240..0x25f: .. 0x24c expo, 0x250, 0x254, 0x258, 0x25c
    b=$(rd $((P + 0x2ac)) 1)       # layer
    c=$(rd $((P + 0x308)) 2)       # 0x308..0x30f: state bytes at 0x30b / 0x30c
    d=$(rd $((P + 0x318)) 2)       # t_wait
    e=$(rd $((P + 0x338)) 2)       # t_exp
    f=$(rd $((0x2fda88 + 0x20)) 3) # cur p_time + counters
    g=$(rd $((P + 0x110)) 2)       # bottom / transition counts
    cur="$a|$b|$c|$d|$e|$f|$g"
    if [ "$cur" != "$last" ]; then echo "$(cut -d' ' -f1 /proc/uptime) $cur" >> $OUT; last=$cur; fi
  fi
  usleep 250000
  cnt=$((cnt+1))
done
