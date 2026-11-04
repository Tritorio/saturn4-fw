# machlib.tcl - the printer's saved machine settings file (step 8.24), shared by sgwebd.tcl and machguard.tcl.
# /media/mmcblk0p1/machine_param_ref.bin = header (00 01, 0x80|k, k-1 x ff, one length byte = file size - 9 mod 256: 23 bytes in the older files,
# 22 in the file the app saved on, so its length is found, not assumed) + JSON text ('{' .. '}') + 00 02 02 +
# CRC-16/KERMIT of the JSON text (poly 0x1021 reflected = 0x8408, init 0, no final xor), low byte first.
# Verified on 3 files of 2 different contents. Runs under the stock jimsh (Jim Tcl 0.80).
# Binary bytes are never put through a Tcl string: header and trailer are written with dd / printf.

# name {lo hi}: the settings the dashboard may change (same table as libsgtap sgmach.c and the page)
set mach_keys {
    i_hold {0 31}  i_run {0 31}  i_change_interval {1 31}  i_change_time {100 10000}
    z_i_hold {0 31}  z_i_run {0 31}  sg_leveing_ths {0 20000}  sg_foregin_triger_ths {0 20000}
    before_n_layer_x_time_slow_mode {0 60000}  after_n_layer_x_time_slow_mode {0 60000}
    before_n_layer_x_time_fast_mode {0 60000}  after_n_layer_x_time_fast_mode {0 60000}
}
set MACH_HDR 23                           ;# only the fallback; the real length = the offset of the first '{' (mach_hdrlen)
set MACH_TRL 5

# header length of a machine settings file: the offset of the first '{' within its first 48 bytes (the header has none); 23 if not found
proc mach_hdrlen {path} {
    global MACH_HDR
    set i 0
    foreach x [split [exec od -An -tu1 -N48 $path]] {
        if {$x eq ""} continue
        if {$x == 123} { return $i }
        incr i
    }
    return $MACH_HDR
}

proc crc16k {s} {
    global crc16tab
    if {![info exists crc16tab]} {
        for {set n 0} {$n < 256} {incr n} {
            set c $n
            for {set i 0} {$i < 8} {incr i} {
                set c [expr {($c & 1) ? (($c >> 1) ^ 0x8408) : ($c >> 1)}]
            }
            lappend crc16tab $c
        }
    }
    set crc 0
    foreach ch [split $s ""] {
        scan $ch %c b
        set crc [expr {($crc >> 8) ^ [lindex $crc16tab [expr {($crc ^ $b) & 255}]]}]
    }
    return $crc
}

# -> {ok json crc_in_file}: ok 1 when header/trailer shape and the CRC are right
proc mach_read {path} {
    global MACH_TRL
    if {![file exists $path]} { return {0 {} 0} }
    set sz [file size $path]
    if {$sz < 60 || $sz > 65536} { return {0 {} 0} }
    set hdr [mach_hdrlen $path]
    if {$hdr < 10 || $hdr > 40 || $sz < $hdr + $MACH_TRL + 20} { return {0 {} 0} }
    set n [expr {$sz - $hdr - $MACH_TRL}]
    set f [open $path r]
    $f seek $hdr
    set json [$f read $n]
    $f close
    if {[string length $json] != $n || [string index $json 0] ne "\{" || [string index $json end] ne "\}"} {
        return [list 0 {} 0]
    }
    set t [exec od -An -tu1 -j [expr {$sz - $MACH_TRL}] $path]
    set t [split [string trim $t]]
    set tr {}
    foreach x $t { if {$x ne ""} { lappend tr $x } }
    if {[llength $tr] != 5 || [lrange $tr 0 2] ne {0 2 2}} { return [list 0 $json 0] }
    set crc [expr {[lindex $tr 3] + 256 * [lindex $tr 4]}]
    return [list [expr {[crc16k $json] == $crc}] $json $crc]
}

# write header (copied from src) + json + trailer to dst
proc mach_write {src json dst} {
    set crc [crc16k $json]
    exec dd if=$src of=$dst bs=[mach_hdrlen $src] count=1 2>/dev/null
    set f [open $dst a]
    $f puts -nonewline $json
    $f close
    exec printf [format {\000\002\002\%03o\%03o} [expr {$crc & 255}] [expr {$crc >> 8}]] >> $dst
}

# apply {name value ...} to the JSON text; error if a name is not exactly once in it
proc mach_patch {json pairs} {
    foreach {k v} $pairs {
        set pat "\"$k\":(-?\[0-9\]+(\\.\[0-9\]+)?)"
        if {[regexp -all $pat $json] != 1} { error "setting $k not found in the saved file" }
        regsub $pat $json "\"$k\":$v" json
    }
    return $json
}
