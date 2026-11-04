#!/bin/sh
# saturn4-fw mod hook, started from /customer/app0.sh after the chitu app.
# Only touches RAM state (mounts, routes, clock, /tmp), except that newly learned
# cloud IPs are appended to /customer/mod/blackhole_ips. Log: /tmp/mod_hook.log
exec >>/tmp/mod_hook.log 2>&1
M=${MOD_DIR:-/customer/mod}
log() { echo "$(date '+%F %T') $*"; }
log "hook start: $(cat $M/VERSION)"

# bind-mount one of our files over a stock file (gone at reboot, reapplied here)
bind() {  # src dst
    if [ ! -f "$1" ]; then
        log "$2: $1 missing, skipped"
    elif grep -q " $2 " /proc/mounts; then
        log "$2: already bound"
    elif mount -o bind "$1" "$2"; then
        log "$2: bind OK"
    else
        log "$2: bind FAIL"
    fi
}

# 8.1b: the MQTT lib resolves via 8.8.8.8 itself (ignores /etc/hosts) -> blackhole
# the cloud IPs. Routes work before the network is up, so the known list goes in first.
blackhole() {  # ip
    ip route add blackhole "$1/32" 2>/dev/null && log "blackhole $1"
}
resolve() {  # name -> IPv4 addresses via real DNS (busybox nslookup skips /etc/hosts)
    nslookup "$1" 2>/dev/null | awk '/^Name:/{n=1} n && /^Address/ {print $NF}' |
        grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'
}
refresh_blackholes() {
    for h in $(cat $M/blackhole_hosts); do
        for a in $(resolve "$h"); do
            grep -qx "$a" $M/blackhole_ips || { echo "$a" >> $M/blackhole_ips; log "learned $h $a"; }
            blackhole "$a"
        done
    done
}

# 7.B: our own root password for the stock telnetd (port 23)
bind $M/shadow /etc/shadow
# 8.1: block Elegoo/Chitu cloud + OTA names for curl (/etc/hosts -> /config/hosts)
bind $M/hosts /config/hosts
# 8.1b: known cloud IPs, before Wi-Fi is up (MQTT connects ~1 s after WAN)
for a in $(cat $M/blackhole_ips); do blackhole "$a"; done

# OTA watch (read only): U-Boot flashes the eMMC copy when this is 1
log "EMMCUpgradeEnable=$(/etc/fw_printenv -n EMMCUpgradeEnable 2>/dev/null)"

# 8.23: the printer's clock is the router's time (user: the printer's internet is blocked at the
# router, so NTP never worked and the clock stayed at 1970). The router's web server (port 80) sends a Date
# header with ITS LOCAL TIME (labelled GMT); the printer takes it as it is, no time zone, so the printer shows
# what the router shows (summer time included). The system clock is therefore the router's wall time.
router_time() {  # -> "YYYY-MM-DD HH:MM:SS" from the default gateway's HTTP Date header
    gw=$(ip route 2>/dev/null | awk '/default/{print $3; exit}')
    [ -n "$gw" ] || return 1
    h=$(wget -T 4 -S -O /dev/null "http://$gw/" 2>&1 | sed -n 's/^ *[Dd]ate: *//p' | head -1 | tr -d '\r')
    [ -n "$h" ] || return 1
    t=$(date -u -D '%a, %d %b %Y %H:%M:%S GMT' -d "$h" '+%Y-%m-%d %H:%M:%S' 2>/dev/null) || return 1
    case "$t" in 202[4-9]-*|20[3-9][0-9]-*) echo "$t" ;; *) return 1 ;; esac
}
set_clock() {  # from the router; only steps when the difference is more than 2 s
    t=$(router_time) || return 1
    new=$(date -u -D '%Y-%m-%d %H:%M:%S' -d "$t" +%s 2>/dev/null) || return 1
    d=$((new - $(date +%s))); [ "$d" -lt 0 ] && d=$((0 - d))
    if [ "$d" -gt 2 ]; then
        [ -n "${CLOCK_DRY:-}" ] && { echo "dry run: would set $t (off by $d s)"; return 0; }
        date -u -s "$t" >/dev/null 2>&1 || return 1
        log "clock set from the router: $t (was off by $d s)"
    fi
    return 0
}

# background: clock + blackhole refresh. The app takes its clock from the (blocked) Chitu server, so we set it
# ourselves (above). If the router gives no usable time for a minute, NTP is tried once per round as a fallback.
(
    tries=0
    until set_clock; do
        tries=$((tries + 1))
        if [ "$tries" -ge 4 ] && ntpd -n -q -p time.google.com >/dev/null 2>&1; then
            log "clock set by ntpd (the router gave no time)"; break
        fi
        sleep 15
    done
    log "clock ready: $(date)"
    while :; do
        refresh_blackholes
        sleep 600
        set_clock || log "clock: no time from the router"
    done
) &
bg=$!

# 8.6: dashboard on the printer. The page (mod/www/index.html) and the live force feed
# (/tmp/sgweb/live.json from libsgtap) are served by the app's own web server on :3030;
# sgwebd takes the writes (tare, auto-tare, calibration k) on :8080. Off without a
# reflash: touch /customer/mod/sgwebd_disable (next boot). Log: /tmp/sgwebd.log
if [ ! -f $M/sgwebd_disable ]; then
    (
        while :; do
            jimsh $M/sgwebd.tcl >> /tmp/sgwebd.log 2>&1 < /dev/null
            log "sgwebd exited ($?), restart in 10 s"
            sleep 10
        done
    ) &
    log "sgwebd started"
fi
# Camera relay (sgcam, port 8091): the printer's RTSP camera server -> fragmented MP4 for the dashboard's Camera card. It does nothing until a
# viewer connects (live view only while the printer is idle, one viewer, the camera session is always closed). It reaches the app's RTSP server
# through the printer's own address, which needs the loopback interface up (it is down in the stock firmware: lo is "noop"; bringing it up only
# changes how local connections work). Off without a reflash: touch /customer/mod/sgcam_disable (next boot). Log: /tmp/sgcam.log
if [ -x $M/sgcam ] && [ ! -f $M/sgcam_disable ]; then
    ifconfig lo 127.0.0.1 up 2>/dev/null && log "lo up"
    (
        while :; do
            $M/sgcam >> /tmp/sgcam.log 2>&1 < /dev/null
            log "sgcam exited ($?), restart in 10 s"
            sleep 10
        done
    ) &
    log "sgcam started"
fi
log "hook done (background loop pid $bg)"
