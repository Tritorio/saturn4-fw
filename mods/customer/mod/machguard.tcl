# machguard.tcl - boot guard for the saved machine settings file (step 8.24). Started by app0.sh BEFORE the app,
# which reads /media/mmcblk0p1/machine_param_ref.bin at start. If that file is not valid (shape + CRC, see
# machlib.tcl) the newest valid backup of the dashboard's machine settings feature (sglog/machine_backups/)
# is put back; nothing is done when the file is fine, missing or no valid backup exists (then only a log
# line in /tmp/machguard.log). Kill switch: /customer/mod/machguard_disable. Env (tests): MACHGUARD_FILE,
# MACHGUARD_BAKDIR.
source [file join [file dirname [info script]] machlib.tcl]
set mfile [env MACHGUARD_FILE /media/mmcblk0p1/machine_param_ref.bin]
set bakdir [env MACHGUARD_BAKDIR /media/mmcblk0p2/sglog/machine_backups]

proc glog {m} {
    set f [open /tmp/machguard.log a]
    $f puts "[clock format [clock seconds] -format {%Y-%m-%d %H:%M:%S}] $m"
    $f close
}

if {![file exists $mfile]} { exit 0 }
lassign [mach_read $mfile] ok
if {$ok} { exit 0 }
glog "saved machine settings file is NOT valid ([file size $mfile] bytes)"
foreach b [lreverse [lsort [glob -nocomplain -directory $bakdir *.bin]]] {
    lassign [mach_read $b] bok
    if {!$bok} { continue }
    set tmp [file dirname $mfile]/.machine_tmp
    if {[catch {file copy -force $b $tmp; file rename -force $tmp $mfile} e]} {
        glog "restoring [file tail $b] failed: $e"
        exit 0
    }
    glog "restored [file tail $b]"
    exit 0
}
glog "no valid backup to restore"
