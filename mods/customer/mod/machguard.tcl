# machguard.tcl - guard for the saved machine settings files (step 8.24, extended after the reset incident).
# Files: /media/mmcblk0p1/machine_param_ref.bin (shape + CRC, see machlib.tcl) and machine_info.bin (size only).
#   (no argument)  BOOT mode, started by app0.sh BEFORE the app: a missing / empty / invalid settings file is replaced by the
#                  newest valid backup (rolling boot backups first, then the dashboard's backups); a valid file is saved as a new
#                  rolling backup when it differs from the newest one; a missing / empty machine_info.bin is restored.
#   snap           (machwatch.sh, every minute) the same backup step, nothing else.
#   restore        put the newest valid backup back over the settings file (and machine_info.bin), the bad file is kept as
#                  boot/suspect_N.bin. Used by machwatch.sh when the app logged "can't find machine parameters".
# Backups: sglog/machine_backups/boot/b0001.bin ... (last 12) AND a mirror on the small boot partition, /media/mmcblk0p1/mg/g0001.bin ...
# (last 3) + info_last.bin: app0.sh runs BEFORE the exfat module is loaded, so at boot sglog (p2) does not exist yet and only the
# p1 mirror can be used; the dashboard's own backups stay in sglog/machine_backups/*.bin. Log: sglog/machguard.log (p1 mg/boot.log
# while p2 is not mounted). Kill switch: /customer/mod/machguard_disable.
# Env (tests): MACHGUARD_FILE, MACHGUARD_INFO, MACHGUARD_BAKDIR, MACHGUARD_LOG.
source [file join [file dirname [info script]] machlib.tcl]
set mfile [env MACHGUARD_FILE /media/mmcblk0p1/machine_param_ref.bin]
set ifile [env MACHGUARD_INFO /media/mmcblk0p1/machine_info.bin]
set bakdir [env MACHGUARD_BAKDIR /media/mmcblk0p2/sglog/machine_backups]
set logf [env MACHGUARD_LOG /media/mmcblk0p2/sglog/machguard.log]
set bootdir $bakdir/boot
set p1dir [file dirname $mfile]/mg
set p2ok [file isdirectory [file dirname $bakdir]]     ;# sglog exists (p2 mounted)
set keep 12
set keep1 3

proc glog {m} {
    global logf p1dir p2ok
    catch {
        if {$p2ok} { set lf $logf } else {
            file mkdir $p1dir
            set lf $p1dir/boot.log
            if {[file exists $lf] && [file size $lf] > 20000} { file delete $lf }
        }
        set f [open $lf a]
        $f puts "[clock format [clock seconds] -format {%Y-%m-%d %H:%M:%S}] $m"
        $f close
    }
}
proc md5 {f} { return [string range [lindex [exec md5sum $f] 0] 0 31] }
proc fsize {f} { if {[file exists $f]} { return [file size $f] } else { return 0 } }

# atomic-ish copy: temp file in the destination folder, then rename
proc safe_copy {src dst} {
    set tmp [file dirname $dst]/.guard_tmp
    file copy -force $src $tmp
    file rename -force $tmp $dst
}

proc roll_list {dir pre} {
    return [lsort [glob -nocomplain -directory $dir ${pre}????.bin]]
}
proc boot_list {} { global bootdir; return [roll_list $bootdir b] }
proc p1_list {} { global p1dir; return [roll_list $p1dir g] }

# numeric "key":value pairs of the JSON text -> number of keys whose value differs
proc json_diff {a b} {
    # a key can occur several times (sub-objects): number the occurrences so that they are compared in order
    foreach {- k v} [regexp -all -inline {"([a-z_0-9]+)":(-?[0-9.]+)} $a] {
        if {![info exists ca($k)]} { set ca($k) 0 }
        set A($k#[incr ca($k)]) $v
    }
    set n 0
    foreach {- k v} [regexp -all -inline {"([a-z_0-9]+)":(-?[0-9.]+)} $b] {
        if {![info exists cb($k)]} { set cb($k) 0 }
        set key $k#[incr cb($k)]
        if {[info exists A($key)] && $A($key) != $v} { incr n }
    }
    return $n
}

# one rolling set: dir/<pre>0001.bin ... ; the (valid) settings file is added when it differs from the newest. -> saved | same
proc roll {dir pre keep json} {
    global mfile
    file mkdir $dir
    set l [roll_list $dir $pre]
    set newest [lindex $l end]
    if {$newest ne "" && [md5 $newest] eq [md5 $mfile]} { return same }
    set nn 1
    if {$newest ne ""} { set nn [expr {[string trimleft [string range [file tail $newest] 1 4] 0] + 1}] }
    set dst $dir/[format ${pre}%04d.bin $nn]
    safe_copy $mfile $dst
    set d "first backup"
    if {$newest ne ""} {
        lassign [mach_read $newest] nok njson
        set d "[json_diff $njson $json] values differ from [file tail $newest]"
    }
    glog "backup $dst ([file size $dst] bytes, $d)"
    set l [roll_list $dir $pre]
    while {[llength $l] > $keep} { file delete [lindex $l 0]; set l [lrange $l 1 end] }
    return saved
}

# backup step: the p1 mirror always, the p2 set when sglog exists. -> saved | same | invalid
proc snap {} {
    global mfile keep keep1 bootdir p1dir p2ok
    lassign [mach_read $mfile] ok json
    if {!$ok} { return invalid }
    set r1 [roll $p1dir g $keep1 $json]
    set r2 same
    if {$p2ok} { set r2 [roll $bootdir b $keep $json] }
    if {$r1 eq "saved" || $r2 eq "saved"} { return saved }
    return same
}

# machine_info.bin is rewritten by the app every minute or so during a print (344 KB): the copy is refreshed at boot (force) and then at most every 30 minutes
proc snap_info {{force 0}} {
    global ifile p1dir
    if {[fsize $ifile] == 0} { return }
    file mkdir $p1dir
    set b $p1dir/info_last.bin
    if {!$force && [file exists $b]} {
        set age [expr {[clock seconds] - [file mtime $b]}]
        if {$age >= 0 && $age < 1800} { return }
    }
    if {[file exists $b] && [md5 $b] eq [md5 $ifile]} { return }
    safe_copy $ifile $b
    glog "backup info_last.bin ([file size $b] bytes)"
}

proc restore_info {} {
    global ifile p1dir
    set b $p1dir/info_last.bin
    if {[fsize $b] == 0} { glog "no machine_info.bin backup"; return }
    if {[catch {safe_copy $b $ifile} e]} { glog "restoring machine_info.bin failed: $e"; return }
    glog "restored machine_info.bin ([file size $ifile] bytes)"
}

# newest valid backup over the settings file. -> 1 when restored
proc restore_best {} {
    global mfile bakdir p2ok
    set cands {}
    if {$p2ok} { set cands [lreverse [boot_list]] }
    foreach b [lreverse [p1_list]] { lappend cands $b }
    if {$p2ok} { foreach b [lreverse [lsort [glob -nocomplain -directory $bakdir *.bin]]] { lappend cands $b } }
    foreach b $cands {
        lassign [mach_read $b] bok
        if {!$bok} { continue }
        if {[catch {safe_copy $b $mfile} e]} { glog "restoring [file tail $b] failed: $e"; return 0 }
        glog "restored [file tail $b]"
        return 1
    }
    glog "no valid backup to restore"
    return 0
}

set mode [lindex [env MACHGUARD_MODE ""] 0]
if {[info exists argv] && [llength $argv] > 0} { set mode [lindex $argv 0] }

if {$mode eq "snap"} {
    catch {snap}
    catch {snap_info}
    exit 0
}

if {$mode eq "restore"} {
    # keep the bad file for the record (last 3), then put the newest valid backup back
    catch {
        set sd [expr {$p2ok ? $bootdir : $p1dir}]
        file mkdir $sd
        set n 1
        foreach s [lsort [glob -nocomplain -directory $sd suspect_*.bin]] { set n [expr {[string trimleft [string range [file tail $s] 8 11] 0] + 1}] }
        if {[file exists $mfile]} { file copy -force $mfile $sd/[format suspect_%04d.bin $n] }
        set s [lsort [glob -nocomplain -directory $sd suspect_*.bin]]
        while {[llength $s] > 3} { file delete [lindex $s 0]; set s [lrange $s 1 end] }
    }
    restore_best
    if {[llength $argv] > 1 && [lindex $argv 1] eq "info"} { restore_info }
    exit 0
}

# BOOT mode
if {[fsize $ifile] == 0} {
    glog "machine_info.bin is [expr {[file exists $ifile] ? {empty} : {missing}}]"
    restore_info
}
if {[fsize $mfile] == 0} {
    glog "saved machine settings file is [expr {[file exists $mfile] ? {empty} : {missing}}]"
    restore_best
} else {
    lassign [mach_read $mfile] ok
    if {!$ok} {
        glog "saved machine settings file is NOT valid ([file size $mfile] bytes)"
        restore_best
    } else {
        set r [catch {snap} e]
        if {$r} { glog "backup step failed: $e" } else { glog "boot: settings file ok ($e)" }
        catch {snap_info 1}
    }
}
