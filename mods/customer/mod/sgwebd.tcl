# sgwebd.tcl - write helper for the dashboard hosted on the printer (step 8.6).
# Runs under the stock jimsh (Jim Tcl 0.80), started by hook.sh. Ports 80 (so that
# http://<printer>/ opens the dashboard) and 8080 (the page's API calls).
# The page itself, the printer API (SDCP websocket), files and the live force feed
# (/tmp/sgweb/live.json, written by libsgtap) all come from the app's own web server on
# port 3030, which is read only. This helper only takes the few writes:
#   GET  /                 -> redirect to the page on :3030
#   POST /api/cmd          body "tare" | "untare" | "auto 0|1" | "k <counts/g>" |
#                          "heat P TPRE H TPRINT [Z]" (pre-heat 0|1, C, heating during print 0|1, C, Z 0|1 = move the plate with the pre-heat) |
#                          "waitmin2 MIN2 N" (second minimum wait, >= 1 s, from layer N to the end; N 0 = off) | "wait DW WH GRAD WIN MIN [DB [WT]]" (dynamic wait 0|1, Wait & heat 0|1,
#                          g/s 0.2-5, s 0.6-3, s >= 1, dynamic wait on bottom layers 0|1,
#                          tare in the wait 0|1) | "settings save|restore|defaults" |
#                          "tilt N A B C D E F S P R T X Y" (N 0-999, speeds 10-144 even, stage
#                          angles E F X Y 0 or 10-120 even) | "tilt stock" |
#                          "slowset A B C D E F" (slow layer set: speeds 10-144 even, angles E F
#                          0 or 10-120 even) | "slowbeep 0|1|test" (dev: beep 2 layers before a slow range; test beep) | "mx on" | "mx off" | "mx set T1 T2 T3 T4 MIRX MIRY" (Multi Exposure, AA variant: quadrant times in s >= 0.1, mirror flags) | "expo off" | "expo B N" (exposure time of the bottom / normal layers in ms 0-60000, "-" = the file's value; allowed without a job: the times wait for the next job) | "waitov 0|1" (ignore the slicer wait after retract while the dynamic wait is on) | "gsync ON AUTO OFFSET_MS" | "gsync search" (release synced to the gauge poll clock) | "slowlayers 0|1" (slow layer lists on/off) | "tilt2 0|1" (two-stage fix) | "joblog 0|1" (CSV per print) | "uisleep IDLE PRINT" (UI loop sleep, ms 5-500)
#                          -> /tmp/sgweb/cmd.<id> for libsgtap (result in live.json "cmd")
#   POST /api/slow         body "<print file name without extension>\n<layer list>": checks the
#                          list (layers 1-based, "20-35" / "120", separated by commas, spaces
#                          or lines, "#" comments; same rules as libsgtap sgtilt.c) and writes
#                          sglog/layertilt/<name>.txt -> {"ok":true,"nl":layers,"nr":ranges}
#   POST /api/slowdel      body "<name>": deletes that list
#   POST /api/fprof        body "<print file name without extension>\n<profile name>" (empty = none): sglog/fileprofile/<name>.txt
#   POST /api/flex         body "on" | "off": backup of the machine settings file (sglog/machine_backups), then the library command "flex on|off" (Flex Plate card)
#   POST /api/profdel      body "<profile name>": deletes sglog/profiles/<name>.txt and the file links to it
#   (the page reads the lists through :3030, /media/mmcblk0p2/sglog/layertilt/)
#   POST /api/machine     body "<label>\n<name value>\n..." (settings of machlib.tcl mach_keys, ints in range):
#                          backup of the saved machine settings file (last 10 in sglog/machine_backups/), the
#                          values patched into it, its CRC recomputed, written + read back -> {"ok":true,"n":..,"backup":".."}
#   POST /api/machine/restore  body "<backup file name>": the backup (CRC checked) becomes the saved file
#   POST /api/machine/delete body "<backup file name>": deletes that backup from sglog/machine_backups
#   POST /api/tldel        body "<job log name>\n...": deletes only the timelapse film of those jobs (the logs stay)
#   POST /api/logdel       body "<job log name>\n...": deletes job logs (<date>_<name>.csv / boot<n>_<name>.csv)
#                          and their .txt, never the running job's; tilt_*.csv are not touched
#   (cmd "mset NAME OLD NEW" = one live machine setting, libsgtap sgmach.c)
#   (cmd "lcdswap NAME RESET" = replace the image of a running lcdshow, RESET 0/1; cmd "lcdshow NAME UV SECS" / "lcdoff": show a stream image on the exposure LCD with the UV off (UV 0) or up to 100 %,
#    fan 100 % on all the time, 1-30 s, idle printer only, libsgtap sgdisp.c; NAME = text | grid | full | a file sglcd/<name>.bin)
#   POST /api/clock        body "YYYY-MM-DD HH:MM:SS" (the browser's wall time): sets the system clock, only while it
#                          is unset (before 2020) and the year is 2024-2039; the router sync in hook.sh still wins later
#   POST /api/calib?k=K    body = calibration details (JSON, < 8 KB): appended to
#                          sglog/calibration.log, then "k K" as above
#   GET  /api/stock/check  verify the cached stock image and that the printer is idle
#   POST /api/stock/restore body "RESTORE STOCK V1.5.9\nauth:<password>": arm eMMC update, then reboot
#   POST /api/password     body "<new password>\nauth:<current password>": changes the printer (= telnet root) password
# PASSWORD: the root password of mod/shadow (bind-mounted over /etc/shadow by hook.sh). A critical endpoint takes the
# password as the LAST line of its body, "auth:<password>" (take_auth), and answers 403 unless auth_check accepts it:
#   set pa [take_auth $body]; set m [auth_check [lindex $pa 1]]; if {$m ne ""} { return [err $c 403 $m] }
# 5 wrong passwords lock the critical endpoints for 5 minutes. The password is never logged or stored.
# LOGIN: every POST except /api/login, /api/logout and /api/clock needs the header
#   X-SG-Token: <32 hex> from POST /api/login (body = the printer password, same 5-wrong lock; answer {"ok":true,"token":..,"ttl":s});
#   missing / unknown = 401 {"error":"login required"}. Tokens live in memory only (a restart ends all logins), at most 20; a login has NO
#   time limit (user: it ends by log out, a restart or a password change). GET /api/session -> {"in":b,"left":s,"ttl":s}
#   (left / ttl = a huge constant), POST /api/logout (this login), POST /api/logout_all.
#   GET  /api/ping
# Answers carry Access-Control-Allow-Origin: * (the page lives on another port).
# Env (tests): SGWEBD_PORT (list), SGWEBD_LIVEDIR, SGWEBD_CSVDIR, SGWEBD_MACHFILE, SGWEBD_SHADOW, SGWEBD_ETCSHADOW.

set ports   [env SGWEBD_PORT {80 8080}]
set livedir [env SGWEBD_LIVEDIR /tmp/sgweb]
set csvdir  [env SGWEBD_CSVDIR /media/mmcblk0p2/sglog]
set page    /customer/mod/www/index.html
set mfile   [env SGWEBD_MACHFILE /media/mmcblk0p1/machine_param_ref.bin]
set mbakdir $csvdir/machine_backups
set stockscript [file join [file dirname [info script]] stock_restore.sh]
set shadowfile [env SGWEBD_SHADOW [file join [file dirname [info script]] shadow]]
set etcshadow [env SGWEBD_ETCSHADOW /etc/shadow]
set auth_fails 0
set auth_until 0
set seq 0
set sess {}          ;# login sessions: token -> expiry (seconds of uptime)
set sess_ttl 315360000   ;# a login has no time limit: its expiry is 10 years of uptime away
set req_tok ""       ;# the X-SG-Token header of the request being handled
source [file join [file dirname [info script]] machlib.tcl]

proc log {msg} {
    puts "[clock format [clock seconds] -format {%Y-%m-%d %H:%M:%S}] $msg"
    flush stdout
}

proc reply {c code body {ctype application/json} {extra {}}} {
    set text [dict get {200 OK 204 {No Content} 302 Found 400 {Bad Request} 401 Unauthorized 403 Forbidden
                        404 {Not Found} 405 {Method Not Allowed} 413 {Payload Too Large}
                        500 {Internal Server Error}} $code]
    $c puts -nonewline "HTTP/1.1 $code $text\r\nContent-Type: $ctype\r\nContent-Length: [string length $body]\r\n"
    $c puts -nonewline "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
    $c puts -nonewline "Access-Control-Allow-Headers: Content-Type, X-SG-Token\r\nCache-Control: no-store\r\nConnection: close\r\n"
    foreach {k v} $extra { $c puts -nonewline "$k: $v\r\n" }
    $c puts -nonewline "\r\n$body"
}

proc err {c code msg} { reply $c $code "{\"error\":\"$msg\"}" }

# the first line of what stock_restore.sh printed, when it is a plain sentence (shown on the page)
proc stock_msg {result} {
    set line [lindex [split $result "\n"] 0]
    if {[regexp {^[A-Za-z0-9 .,'-]{1,80}$} $line] && ![string match "child process*" $line]} { return $line }
    return "stock restore unavailable"
}

# ---- printer password = the root password in mod/shadow ($5$ = SHA-256 crypt, made by busybox mkpasswd) ----
proc shadow_read {} {
    global shadowfile
    if {[catch {set f [open $shadowfile r]; set d [read $f]; close $f}]} { return "" }
    return $d
}

proc shadow_root_hash {} {
    foreach l [split [shadow_read] "\n"] {
        if {[string match "root:*" $l]} { return [lindex [split $l :] 1] }
    }
    return ""
}

# the hash of pw with the given salt ("" when mkpasswd fails); the password goes in on stdin, never in a command line
proc pw_hash {pw salt} {
    if {[catch {exec mkpasswd -P 0 -m sha256 -S $salt << "$pw\n"} h]} { return "" }
    return [string trim $h]
}

# "" = the password is right, else the message for the page
proc auth_check {pw} {
    global auth_fails auth_until
    set now [clock seconds]
    if {$now < $auth_until} { return "too many wrong passwords, try again in [expr {($auth_until - $now + 59) / 60}] min" }
    if {$pw eq ""} { return "password required" }
    set h [shadow_root_hash]
    if {![regexp {^\$5\$([^$]+)\$[./0-9A-Za-z]+$} $h -> salt]} {
        log "password check unavailable: no usable root hash"
        return "password check unavailable"
    }
    set got [pw_hash $pw $salt]
    if {$got eq ""} { log "password check unavailable: mkpasswd failed"; return "password check unavailable" }
    if {$got eq $h} { set auth_fails 0; return "" }
    incr auth_fails
    log "wrong password ($auth_fails)"
    if {$auth_fails >= 5} {
        set auth_until [expr {$now + 300}]; set auth_fails 0
        log "5 wrong passwords: critical actions locked for 5 min"
    }
    return "wrong password"
}

# a protected body is "<payload>\nauth:<password>" -> {payload password}
proc take_auth {body} {
    set i [string last "\nauth:" $body]
    if {$i < 0} { return [list $body ""] }
    return [list [string range $body 0 $i-1] [string range $body [expr {$i + 6}] end]]
}

# new root password: "" = done, else the error. Atomic (temp file + rename), the old file stays as shadow.bak.
proc pw_set {new} {
    global shadowfile etcshadow
    if {[string length $new] < 1 || [string length $new] > 64 || [string first "\n" $new] >= 0 || [string first "\r" $new] >= 0} {
        return "the new password needs 1 to 64 characters"
    }
    if {[catch {string map {" " ""} [string trim [exec od -An -tx1 -N8 /dev/urandom]]} salt] || [string length $salt] != 16} {
        set salt [format %016x [clock microseconds]]
    }
    set h [pw_hash $new $salt]
    if {![regexp {^\$5\$[^$]+\$[./0-9A-Za-z]+$} $h]} { return "hashing failed" }
    set out {}
    set found 0
    foreach l [split [shadow_read] "\n"] {
        if {[string match "root:*" $l]} {
            set f [split $l :]
            set l [join [lreplace $f 1 1 $h] :]
            set found 1
        }
        lappend out $l
    }
    if {!$found} { return "no root entry in the shadow file" }
    set tmp $shadowfile.new
    if {[catch {
        exec cp $shadowfile $shadowfile.bak
        set f [open $tmp w]; puts -nonewline $f [join $out "\n"]; close $f
        exec chmod 600 $tmp $shadowfile.bak
        exec sync
    } e]} { catch {file delete $tmp}; return "could not write the shadow file ($e)" }
    if {[shadow_root_hash_of $tmp] ne $h} { catch {file delete $tmp}; return "read-back of the new shadow file failed" }
    if {[catch {exec mv -f $tmp $shadowfile; exec sync} e]} { return "could not replace the shadow file ($e)" }
    # /etc/shadow is a bind mount of the OLD file (now replaced, so a second bind cannot stack on it): unmount, bind the new one.
    # A power cut here loses nothing: the rename above is atomic and hook.sh binds the file again at the next boot.
    if {![catch {exec grep -q " $etcshadow " /proc/mounts}]} {
        catch {exec umount $etcshadow}
        set bound 0
        for {set k 0} {$k < 5 && !$bound} {incr k} {
            if {[catch {exec mount -o bind $shadowfile $etcshadow} e]} { log "password: bind attempt $k failed: $e"; after 200 } else { set bound 1 }
        }
        if {!$bound} { return "saved, but /etc/shadow was not updated: restart the printer" }
    }
    return ""
}

proc shadow_root_hash_of {file} {
    if {[catch {set f [open $file r]; set d [read $f]; close $f}]} { return "" }
    foreach l [split $d "\n"] {
        if {[string match "root:*" $l]} { return [lindex [split $l :] 1] }
    }
    return ""
}

# ---- login sessions (for the writes of the page): a random token kept in memory only, sent as X-SG-Token ----
proc mono {} {
    if {[catch {set f [open /proc/uptime r]; set d [read $f]; close $f}]} { return [clock seconds] }
    return [expr {int([lindex $d 0])}]
}

proc sess_purge {} {
    global sess
    set now [mono]
    foreach {t e} $sess { if {$e <= $now} { dict unset sess $t } }
}

# the seconds left of the login of this request (0 = not logged in); touch = restart the time limit
proc sess_left {touch} {
    global sess sess_ttl req_tok
    sess_purge
    if {$req_tok eq "" || ![dict exists $sess $req_tok]} { return 0 }
    if {$touch} { dict set sess $req_tok [expr {[mono] + $sess_ttl}] }
    return [expr {[dict get $sess $req_tok] - [mono]}]
}

proc sess_new {} {
    global sess sess_ttl
    sess_purge
    if {[catch {string map {" " ""} [string trim [exec od -An -tx1 -N16 /dev/urandom]]} tok] || ![regexp {^[0-9a-f]{32}$} $tok]} { return "" }
    while {[dict size $sess] >= 20} {                      ;# never more than 20 logins: the oldest goes
        set oldt ""; set olde 0
        foreach {t e} $sess { if {$oldt eq "" || $e < $olde} { set oldt $t; set olde $e } }
        dict unset sess $oldt
    }
    dict set sess $tok [expr {[mono] + $sess_ttl}]
    return $tok
}

# a command for libsgtap: temp file + rename, so it never reads half a line
proc send_cmd {line} {
    global livedir seq
    file mkdir $livedir
    # fixed width, so the ids sort in order (jimsh's format ignores zero padding)
    set id [clock milliseconds][string range 0000[expr {[incr seq] % 10000}] end-3 end]
    set f [open $livedir/.tmp$id w]
    $f puts $line
    $f close
    file rename $livedir/.tmp$id $livedir/cmd.$id
    log "cmd $id: $line"
    return $id
}

proc valid_k {k} {
    if {![regexp {^-?[0-9]+(\.[0-9]+)?$} $k]} { return 0 }
    set a [expr {abs($k)}]
    expr {$a >= 1 && $a <= 100000}
}

# vat temperatures: same range as libsgtap (sgheat.c)
proc valid_t {t max} {
    expr {[regexp {^[1-9][0-9]$} $t] && $t >= 20 && $t <= $max}
}

# dynamic wait: same ranges as libsgtap (sgwait.c)
proc valid_wait {g w m} {
    expr {$g >= 0.2 && $g <= 5 && $w >= 0.6 && $w <= 3 && $m >= 1}
}

# tilt profile: same ranges as libsgtap (sgtilt.c tp_valid)
proc valid_tilt {vals} {
    if {[llength $vals] != 13} { return 0 }
    set i 0
    foreach v $vals {
        if {![regexp {^(0|[1-9][0-9]{0,2})$} $v]} { return 0 }   ;# no leading 0 (octal in expr)
        if {$i == 0} {
            if {$v > 999} { return 0 }
        } elseif {$i in {5 6 11 12}} {
            if {$v % 2 || ($v != 0 && ($v < 10 || $v > 120))} { return 0 }
        } elseif {$v % 2 || $v < 10 || $v > 144} { return 0 }
        incr i
    }
    return 1
}

# slow layers: same set rules as libsgtap (sgtilt.c sl_valid)
proc valid_slowset {vals} {
    if {[llength $vals] != 6} { return 0 }
    set i 0
    foreach v $vals {
        if {![regexp {^(0|[1-9][0-9]{0,2})$} $v] || $v % 2} { return 0 }
        if {$i >= 4} {
            if {$v != 0 && ($v < 10 || $v > 120)} { return 0 }
        } elseif {$v < 10 || $v > 144} { return 0 }
        incr i
    }
    return 1
}

# layer list -> {layers ranges} or error "line N: ..." (sgtilt.c sl_parse)
proc slow_parse {text} {
    set rs {}
    set ln 0
    foreach line [split $text \n] {
        incr ln
        set i [string first # $line]
        if {$i >= 0} { set line [string range $line 0 $i-1] }
        foreach tok [split [string map {, " " \t " " \r " "} $line] " "] {
            if {$tok eq ""} continue
            if {[regexp {^([1-9][0-9]{0,5})(-([1-9][0-9]{0,5}))?$} $tok -> a - b]} {
                if {$b eq ""} { set b $a }
                if {$a <= 100000 && $b <= 100000 && $b >= $a} {
                    lappend rs [list $a $b]
                    continue
                }
            }
            return -code error "line $ln: '$tok' is not a layer or range"
        }
    }
    if {[llength $rs] > 512} { return -code error "more than 512 ranges" }
    set m {}
    foreach r [lsort -integer -index 0 $rs] {
        lassign $r a b
        if {[llength $m] && $a <= [lindex $m end 1] + 1} {
            if {$b > [lindex $m end 1]} { lset m end 1 $b }
        } else {
            lappend m [list $a $b]
        }
    }
    set nl 0
    foreach r $m { incr nl [expr {[lindex $r 1] - [lindex $r 0] + 1}] }
    list $nl [llength $m]
}

# print file name without extension: one path element, no control characters
proc valid_stem {n} {
    expr {$n ne "" && [string length $n] <= 150 && ![regexp {[/\\\x01-\x1f]} $n] &&
          [string index $n 0] ne "."}
}

# settings profile name (sgwait.c prof_name): letters, digits, space - _ . ; 1-32 characters, no space at either end, not starting with a dot;
# "Standard" is the unnamed snapshot (no name in the command)
proc valid_profile {n} {
    expr {[regexp {^[A-Za-z0-9_.-]([A-Za-z0-9 _.-]{0,30}[A-Za-z0-9_.-])?$} $n] && [string index $n 0] ne "." && [string tolower $n] ne "standard"}
}

# one backup of the saved machine settings file; keeps the newest 10
proc mach_backup {label} {
    global mfile mbakdir
    file mkdir $mbakdir
    regsub -all {[^A-Za-z0-9_-]} $label _ label
    set name [clock format [clock seconds] -format {%Y%m%d_%H%M%S}]_[string range $label 0 39].bin
    file copy -force $mfile $mbakdir/$name
    set all [lsort [glob -nocomplain -directory $mbakdir *.bin]]
    while {[llength $all] > 10} { file delete [lindex $all 0]; set all [lrange $all 1 end] }
    return $name
}

proc handle {c method path query host body origin} {
    global csvdir page mfile mbakdir mach_keys livedir stockscript sess sess_ttl req_tok
    if {$method eq "OPTIONS"} { return [reply $c 204 ""] }
    # every write needs a login, except the login itself, the logout and the browser clock (only while it is unset)
    if {$method eq "POST" && [lsearch {/api/login /api/logout /api/clock} $path] < 0 && ![sess_left 1]} {
        return [err $c 401 "login required"]
    }
    switch -- "$method $path" {
        "GET /api/session" {
            set left [sess_left 0]
            reply $c 200 "{\"in\":[expr {$left > 0 ? "true" : "false"}],\"left\":$left,\"ttl\":$sess_ttl}"
        }
        "POST /api/login" {
            if {$origin ne "http://$host:3030"} { return [err $c 403 "open the printer dashboard to log in"] }
            set m [auth_check [string trimright $body "\r\n"]]
            if {$m ne ""} { return [err $c 403 $m] }
            set tok [sess_new]
            if {$tok eq ""} { return [err $c 500 "could not make a login token"] }
            log "login"
            reply $c 200 "{\"ok\":true,\"token\":\"$tok\",\"ttl\":$sess_ttl}"
        }
        "POST /api/logout" {
            if {$req_tok ne ""} { catch {dict unset sess $req_tok} }
            reply $c 200 {{"ok":true}}
        }
        "POST /api/logout_all" {
            set sess {}
            log "all logins ended"
            reply $c 200 {{"ok":true}}
        }
        "GET /" - "GET /index.html" {
            reply $c 302 "" text/plain [list Location "http://$host:3030$page"]
        }
        "GET /api/ping" { reply $c 200 {{"ok":true}} }
        "GET /api/stock/check" {
            if {[catch {exec /bin/sh $stockscript check} result]} {
                log "stock restore check: $result"
                return [err $c 400 [stock_msg $result]]
            }
            reply $c 200 "{\"ok\":true,\"version\":\"V1.5.9\",\"md5\":\"$result\"}"
        }
        "POST /api/stock/restore" {
            if {$origin ne "http://$host:3030"} { return [err $c 403 "open the printer dashboard to restore stock"] }
            set pa [take_auth $body]
            if {[lindex $pa 0] ne "RESTORE STOCK V1.5.9"} { return [err $c 400 "confirmation text required"] }
            set m [auth_check [lindex $pa 1]]
            if {$m ne ""} { return [err $c 403 $m] }
            if {[catch {exec /bin/sh $stockscript arm} result]} {
                log "stock restore arm: $result"
                return [err $c 400 [stock_msg $result]]
            }
            log "stock restore armed: $result; reboot in 2 s"
            after 2000 {
                if {[catch {exec /bin/sync; exec /sbin/reboot} rbres]} {
                    # never leave the printer armed without a reboot: the next power-on would flash
                    log "stock restore: reboot failed ($rbres), disarming"
                    catch {exec /bin/sh $stockscript disarm} dres
                    log "stock restore disarm: $dres"
                }
            }
            reply $c 200 {{"ok":true,"rebooting":true}}
        }
        "POST /api/password" {
            if {$origin ne "http://$host:3030"} { return [err $c 403 "open the printer dashboard to change the password"] }
            set pa [take_auth $body]
            set m [auth_check [lindex $pa 1]]
            if {$m ne ""} { return [err $c 403 $m] }
            set m [pw_set [lindex $pa 0]]
            if {$m ne ""} { log "password change failed: $m"; return [err $c 400 $m] }
            log "password changed"
            set sess {}
            reply $c 200 {{"ok":true,"loggedout":true}}
        }
        "POST /api/cmd" {
            set cmd [string trim $body]
            set dw ""
            if {[regexp {^tilt ([0-9][0-9 ]*[0-9])$} $cmd -> tv]} {
                if {![valid_tilt [split $tv " "]]} { return [err $c 400 "bad command"] }
            } elseif {$cmd eq "tilt stock"} {
            } elseif {[regexp {^slowset ([0-9][0-9 ]*[0-9])$} $cmd -> sv]} {
                if {![valid_slowset [split $sv " "]]} { return [err $c 400 "bad command"] }
            } elseif {[regexp {^mset ([a-z_]+) (-?[0-9]+) (-?[0-9]+)$} $cmd -> mk mo mn]} {
                if {![dict exists $mach_keys $mk] || $mn < [lindex [dict get $mach_keys $mk] 0] ||
                    $mn > [lindex [dict get $mach_keys $mk] 1]} { return [err $c 400 "bad command"] }
            } elseif {[regexp {^lcdshow ([a-z0-9_-]{1,32}) ([0-9]{1,3}) ([0-9]{1,3})$} $cmd -> ln ld ls]} {
                if {$ld > 100 || $ls < 1 || $ls > 30} { return [err $c 400 "bad command"] }
            } elseif {[regexp {^lcdswap ([a-z0-9_-]{1,32}) [01]$} $cmd]} {
            } elseif {[regexp {^mix ([0-9]{1,4})$} $cmd]} {
            } elseif {$cmd eq "resin off" || ([regexp {^resin (.+)$} $cmd -> rpn] && [valid_profile $rpn])} {
            } elseif {$cmd eq "tl on" || $cmd eq "tl off"} {
            } elseif {[regexp {^waitov [01]$} $cmd]} {
            } elseif {$cmd eq "expo off"} {
            } elseif {[regexp {^expo (-|[0-9]{1,5}) (-|[0-9]{1,5})$} $cmd -> eb en]} {
                if {($eb eq "-" && $en eq "-") || ($eb ne "-" && $eb > 60000) || ($en ne "-" && $en > 60000)} { return [err $c 400 "bad command"] }
            } elseif {$cmd eq "mx on" || $cmd eq "mx off"} {
            } elseif {[regexp {^mx from ([1-9][0-9]{0,4})$} $cmd]} {
            } elseif {[regexp {^mx set ([0-9]{1,5}(\.[0-9]{1,3})?) ([0-9]{1,5}(\.[0-9]{1,3})?) ([0-9]{1,5}(\.[0-9]{1,3})?) ([0-9]{1,5}(\.[0-9]{1,3})?) [01] [01]$} $cmd]} {
                foreach mxt [lrange [split $cmd " "] 2 5] { if {$mxt < 0.1} { return [err $c 400 "bad command"] } }
            } elseif {[regexp {^tiltmode [01]$} $cmd]} {
            } elseif {[regexp {^xmove ([0-9]{1,3})(\.[0-9])?$} $cmd -> xi xd]} {
                if {$xi > 140 || ($xi == 140 && $xd ne "" && $xd ne ".0")} { return [err $c 400 "bad command"] }
            } elseif {[regexp {^dynlift ([01]) ([0-9]{3,5}) ([0-9]{2,5}) ([0-9]{1,2}) ([0-9]{1,3})( ([0-9]{1,6}))?$} $cmd -> dlo dll dlw dls dlf dlx dlst]} {
                if {$dll < 100 || $dll > 20000 || $dlw < 50 || $dlw >= $dll || $dls < 1 || $dls > 90 || $dlf < 1 || $dlf > 100} { return [err $c 400 "bad command"] }
                if {$dlst ne "" && ([scan $dlst %d dlsn] != 1 || $dlsn < 1 || $dlsn > 100000)} { return [err $c 400 "bad command"] }
            } elseif {$cmd eq "dynlift reset"} {
            } elseif {[regexp {^gsync ([01] [01] [0-9]{1,3}|search)$} $cmd]} {
            } elseif {[regexp {^(slowlayers|tilt2|joblog) [01]$} $cmd]} {
            } elseif {[regexp {^slowbeep (0|1|test)$} $cmd]} {
            } elseif {$cmd eq "lcdoff"} {
            } elseif {[regexp {^uisleep ([1-9][0-9]{0,2}) ([1-9][0-9]{0,2})$} $cmd -> ui up]} {
                if {$ui < 5 || $ui > 500 || $up < 5 || $up > 500} { return [err $c 400 "bad command"] }
            } elseif {[regexp {^settings (save|restore) (.+)$} $cmd -> - pn]} {
                if {![valid_profile $pn]} { return [err $c 400 "bad profile name"] }
            } elseif {[regexp {^waitmin2 ([0-9]+\.?[0-9]*) ([0-9]{1,5})$} $cmd -> m2 m2n]} {
                if {$m2 < 1} { return [err $c 400 "bad command"] }
            } elseif {[regexp {^wait ([01]) ([01]) ([0-9]+\.?[0-9]*) ([0-9]+\.?[0-9]*) ([0-9]+\.?[0-9]*)( [01])?( [01])?$} $cmd -> dw wh wg ww wm db wt]} {
                if {![valid_wait $wg $ww $wm]} { return [err $c 400 "bad command"] }
            } elseif {![regexp {^(tare|untare|auto [01]|k (-?[0-9.]+)|heat [01] ([0-9]+) [01] ([0-9]+)( [01])?|settings (save|restore|defaults))$} $cmd -> - k tpre tprint - -] ||
                ($k ne "" && ![valid_k $k]) ||
                ($tpre ne "" && (![valid_t $tpre 40] || ![valid_t $tprint 50]))} {
                return [err $c 400 "bad command"]
            }
            reply $c 200 "{\"ok\":true,\"id\":\"[send_cmd $cmd]\"}"
        }
        "POST /api/slow" {
            set i [string first \n $body]
            set name [string trimright [string range $body 0 $i-1] \r]
            if {$i < 0 || ![valid_stem $name]} { return [err $c 400 "bad file name"] }
            set text [string range $body $i+1 end]
            if {[catch {slow_parse $text} res]} { return [err $c 400 $res] }
            file mkdir $csvdir/layertilt
            set f [open $csvdir/layertilt/.tmp w]
            $f puts -nonewline $text
            $f close
            file rename -force $csvdir/layertilt/.tmp $csvdir/layertilt/$name.txt
            log "slow layers $name: [lindex $res 0] layers, [lindex $res 1] ranges"
            reply $c 200 "{\"ok\":true,\"nl\":[lindex $res 0],\"nr\":[lindex $res 1]}"
        }
        "POST /api/flex" {
            set w [string trim $body]
            if {$w ne "on" && $w ne "off"} { return [err $c 400 "bad flex command"] }
            if {![file exists $mfile]} { return [err $c 400 "no machine settings file"] }
            set bak [mach_backup flex_$w]
            log "flex plate $w: backup $bak"
            reply $c 200 "{\"ok\":true,\"id\":\"[send_cmd "flex $w"]\",\"backup\":\"$bak\"}"
        }
        "POST /api/resin" {
            set i [string first \n $body]
            set name [string trimright [string range $body 0 $i-1] \r]
            if {$i < 0 || ![valid_profile $name] || $name eq "off"} { return [err $c 400 "bad resin name"] }
            set rows {}
            set seen {}
            foreach ln [split [string range $body $i+1 end] \n] {
                set ln [string trim $ln]
                if {$ln eq ""} continue
                if {![regexp {^([0-9]{1,4}) ([0-9]{1,5}) ([0-9]{1,5})$} $ln -> um b n]} { return [err $c 400 "bad row: $ln"] }
                foreach v {um b n} { set x [string trimleft [set $v] 0]; if {$x eq ""} { set x 0 }; set $v $x }
                if {$um < 1 || $um > 2000 || $b > 60000 || $n > 60000} { return [err $c 400 "row out of range: $ln"] }
                if {$um in $seen} { return [err $c 400 "thickness $um um twice"] }
                lappend seen $um
                lappend rows "$um $b $n"
            }
            if {[llength $rows] < 1 || [llength $rows] > 16} { return [err $c 400 "1 to 16 rows"] }
            file mkdir $csvdir/resins
            set f [open $csvdir/resins/.tmp w]
            puts $f [join $rows \n]
            close $f
            file rename -force $csvdir/resins/.tmp $csvdir/resins/$name.txt
            log "resin profile $name: [llength $rows] rows"
            reply $c 200 {{"ok":true}}
        }
        "POST /api/resindel" {
            set name [string trim $body]
            if {![valid_profile $name]} { return [err $c 400 "bad resin name"] }
            file delete $csvdir/resins/$name.txt
            log "resin profile $name: deleted"
            reply $c 200 {{"ok":true}}
        }
        "POST /api/slowdel" {
            set name [string trim $body]
            if {![valid_stem $name]} { return [err $c 400 "bad file name"] }
            file delete $csvdir/layertilt/$name.txt
            log "slow layers $name: deleted"
            reply $c 200 {{"ok":true}}
        }
        "POST /api/fprof" {
            set i [string first \n $body]
            set name [string trimright [string range $body 0 $i-1] \r]
            set prof [string trim [string range $body $i+1 end]]
            if {$i < 0 || ![valid_stem $name] || ($prof ne "" && ![valid_profile $prof] && $prof ne "Standard")} { return [err $c 400 "bad file or profile name"] }
            file mkdir $csvdir/fileprofile
            if {$prof eq ""} {
                file delete $csvdir/fileprofile/$name.txt
            } else {
                set f [open $csvdir/fileprofile/.tmp w]
                puts $f $prof
                close $f
                file rename -force $csvdir/fileprofile/.tmp $csvdir/fileprofile/$name.txt
            }
            log "file profile $name: [expr {$prof eq "" ? "none" : $prof}]"
            reply $c 200 {{"ok":true}}
        }
        "POST /api/profdel" {
            set name [string trim $body]
            if {![valid_profile $name]} { return [err $c 400 "bad profile name"] }
            file delete $csvdir/profiles/$name.txt
            if {[file isdirectory $csvdir/fileprofile]} {
                foreach f [glob -nocomplain -directory $csvdir/fileprofile *.txt] {
                    if {![catch {set h [open $f]; set v [string trim [read $h]]; close $h}] && $v eq $name} { file delete $f }
                }
            }
            log "profile $name: deleted"
            reply $c 200 {{"ok":true}}
        }
        "POST /api/machine" {
            set lines [split [string trim $body] \n]
            set label [string trim [lindex $lines 0]]
            set pairs {}
            foreach l [lrange $lines 1 end] {
                set l [string trim $l]
                if {$l eq ""} continue
                if {![regexp {^([a-z_]+) (-?[0-9]+)$} $l -> k v] || ![dict exists $mach_keys $k]} {
                    return [err $c 400 "bad setting line"]
                }
                lassign [dict get $mach_keys $k] lo hi
                if {$v < $lo || $v > $hi} { return [err $c 400 "$k outside $lo..$hi"] }
                lappend pairs $k $v
            }
            if {[llength $pairs] == 0 || [llength $pairs] > 80} { return [err $c 400 "no settings"] }
            lassign [mach_read $mfile] ok json crc
            if {!$ok} { return [err $c 400 "the saved settings file is not valid, nothing written"] }
            if {[catch {mach_patch $json $pairs} njson]} { return [err $c 400 $njson] }
            set bak [mach_backup $label]
            set tmp [file dirname $mfile]/.machine_tmp
            if {[catch {
                mach_write $mfile $njson $tmp
                lassign [mach_read $tmp] ok2 j2 c2
            } e] || !$ok2 || $j2 ne $njson} {
                catch {file delete $tmp}
                return [err $c 500 "check after writing failed, nothing changed"]
            }
            file rename -force $tmp $mfile
            log "machine settings: [expr {[llength $pairs] / 2}] changed ($pairs), backup $bak"
            reply $c 200 "{\"ok\":true,\"n\":[expr {[llength $pairs] / 2}],\"backup\":\"$bak\"}"
        }
        "POST /api/machine/restore" {
            set name [string trim $body]
            if {![regexp {^[0-9]{8}_[0-9]{6}_[A-Za-z0-9_-]{0,40}\.bin$} $name]} { return [err $c 400 "bad backup name"] }
            lassign [mach_read $mbakdir/$name] ok json crc
            if {!$ok} { return [err $c 400 "that backup is not valid"] }
            set bak [mach_backup before_restore]
            set tmp [file dirname $mfile]/.machine_tmp
            file copy -force $mbakdir/$name $tmp
            lassign [mach_read $tmp] ok2 j2 c2
            if {!$ok2} { catch {file delete $tmp}; return [err $c 500 "check after copying failed, nothing changed"] }
            file rename -force $tmp $mfile
            log "machine settings: restored $name (before: $bak)"
            reply $c 200 "{\"ok\":true,\"backup\":\"$bak\"}"
        }
        "POST /api/machine/delete" {
            set name [string trim $body]
            if {![regexp {^[0-9]{8}_[0-9]{6}_[A-Za-z0-9_-]{0,40}\.bin$} $name]} { return [err $c 400 "bad backup name"] }
            if {![file isfile $mbakdir/$name]} { return [err $c 404 "no such backup"] }
            file delete $mbakdir/$name
            log "machine settings: deleted backup $name"
            reply $c 200 "{\"ok\":true}"
        }
        "POST /api/logdel" {
            set running ""
            catch {
                set f [open $livedir/live.json]
                set txt [$f read]
                $f close
                regexp {"job":\{"active":1,"phase":"[^"]*","layer":-?[0-9]+,"csv":"([^"]*)"} $txt -> running
            }
            set n 0
            set skipped {}
            foreach l [split $body \n] {
                set name [string trim $l]
                if {$name eq ""} continue
                if {![regexp {^(boot[0-9]+|[0-9]{8}_[0-9]{6})_[A-Za-z0-9_.-]+\.csv$} $name] || $name eq $running} {
                    lappend skipped $name
                    continue
                }
                file delete $csvdir/$name
                file delete $csvdir/[file rootname $name].txt
                file delete $csvdir/[file rootname $name].h264
                file delete /media/mmcblk0p3/.timelapse/[file rootname $name].h264
                incr n
            }
            log "job logs deleted: $n, skipped: [llength $skipped]"
            reply $c 200 "{\"ok\":true,\"deleted\":$n,\"skipped\":[llength $skipped]}"
        }
        "POST /api/tldel" {
            set running ""
            catch {
                set f [open $livedir/live.json]
                set txt [$f read]
                $f close
                regexp {"job":\{"active":1,"phase":"[^"]*","layer":-?[0-9]+,"csv":"([^"]*)"} $txt -> running
            }
            set n 0
            set skipped {}
            foreach l [split $body \n] {
                set name [string trim $l]
                if {$name eq ""} continue
                if {![regexp {^(boot[0-9]+|[0-9]{8}_[0-9]{6})_[A-Za-z0-9_.-]+\.csv$} $name] || $name eq $running} {
                    lappend skipped $name
                    continue
                }
                set had 0
                foreach fn [list $csvdir/[file rootname $name].h264 /media/mmcblk0p3/.timelapse/[file rootname $name].h264] {
                    if {[file exists $fn]} { file delete $fn; set had 1 }
                }
                incr n $had
            }
            log "timelapses deleted: $n, skipped: [llength $skipped]"
            reply $c 200 "{\"ok\":true,\"deleted\":$n,\"skipped\":[llength $skipped]}"
        }
        "POST /api/clock" {
            set t [string trim $body]
            if {![regexp {^(20[0-9][0-9])-([01][0-9])-([0-3][0-9]) ([0-2][0-9]):([0-5][0-9]):([0-5][0-9])$} $t -> y mo d h mi se] ||
                $y < 2024 || $y > 2039 || $mo < 1 || $mo > 12 || $d < 1 || $d > 31 || $h > 23} {
                return [err $c 400 "bad time"]
            }
            if {[clock seconds] >= 1577836800} { return [reply $c 200 {{"ok":true,"set":false}}] }
            if {[catch {exec date -u -s $t}]} { return [err $c 500 "could not set the clock"] }
            log "clock set from the browser: $t"
            reply $c 200 {{"ok":true,"set":true}}
        }
        "POST /api/calib" {
            if {![regexp {(^|&)k=(-?[0-9.]+)(&|$)} $query -> - k] || ![valid_k $k]} {
                return [err $c 400 "bad k"]
            }
            file mkdir $csvdir
            set f [open $csvdir/calibration.log a]
            $f puts "[clock format [clock seconds] -format {%Y-%m-%d %H:%M:%S} -gmt 1] k=$k [string map {\n { } \r {}} $body]"
            $f close
            reply $c 200 "{\"ok\":true,\"id\":\"[send_cmd "k $k"]\"}"
        }
        default {
            if {$path in {/ /index.html /api/ping /api/stock/check /api/stock/restore /api/password /api/cmd /api/calib /api/slow /api/slowdel /api/resin /api/resindel /api/fprof /api/profdel /api/flex /api/machine /api/machine/restore /api/machine/delete /api/logdel /api/tldel /api/clock}} {
                err $c 405 "method not allowed"
            } else {
                err $c 404 "not found"
            }
        }
    }
}

# non-blocking reads (a browser may open a connection and send nothing for a while);
# a request is handled once its headers and body are complete, then the connection closes
proc drop {c} {
    global buf
    catch {$c close}
    catch {unset buf($c)}
}

proc on_read {c} {
    global buf
    if {[catch {$c read} d] || ($d eq "" && [$c eof])} { return [drop $c] }
    append buf($c) $d
    set r $buf($c)
    set i [string first "\r\n\r\n" $r]
    if {$i < 0} {
        if {[string length $r] > 16384} { drop $c }
        return
    }
    set head [split [string range $r 0 $i-1] \n]
    set len 0
    set host ""
    set origin ""
    global req_tok
    set req_tok ""
    foreach h [lrange $head 1 end] {
        regexp -nocase {^x-sg-token: *([0-9a-f]{32})} $h -> req_tok
        regexp -nocase {^content-length: *([0-9]+)} $h -> len
        regexp -nocase {^host: *([^:\r]+)} $h -> host
        regexp -nocase {^origin: *(https?://[^\r ]+)} $h -> origin
    }
    if {$len <= 8192 && [string length $r] < $i + 4 + $len} { return }
    $c readable {}
    $c ndelay 0
    if {[catch {
        if {$len > 8192} {
            err $c 413 "body too large"
        } elseif {![regexp {^([A-Z]+) ([^ ?]*)(\?([^ ]*))? HTTP/1\.[01]$} [string trimright [lindex $head 0] \r] -> method path - query]} {
            err $c 400 "bad request"
        } else {
            handle $c $method $path $query $host [string range $r $i+4 [expr {$i + 3 + $len}]] $origin
        }
    } e]} { log "error: $e" }
    drop $c
}

proc accept {srv} {
    if {[catch {$srv accept} c]} { return }
    $c ndelay 1
    $c readable [list on_read $c]
    after 10000 [list drop $c]
}

foreach port $ports {
    set srv [socket stream.server 0.0.0.0:$port]
    $srv readable [list accept $srv]
    log "sgwebd listening on $port"
}
vwait forever
