#!/bin/sh
# Restore the exact stock V1.5.9 image already cached on the printer's eMMC.
# U-Boot's sdstar command reads /ChituUpgrade.bin from mmc 0 when
# EMMCUpgradeEnable=1 (the app's own OTA path sets the same variable the same
# way: /etc/fw_setenv EMMCUpgradeEnable 1). This script arms that existing update
# path; the caller sends its HTTP response, syncs and reboots. U-Boot clears the
# flag itself after a successful flash; after a failed flash it stays 1 (retry at
# every boot, a stock USB stick still wins because usbstar runs first).
#   check   verify image + printer state, print the md5
#   arm     check, then set EMMCUpgradeEnable=1 (read back)
#   disarm  set EMMCUpgradeEnable=0 (used when the reboot after arm fails)
set -eu

image=${STOCK_IMAGE:-/media/mmcblk0p1/ChituUpgrade.bin}
live=${STOCK_LIVE:-/tmp/sgweb/live.json}
fw_printenv=${STOCK_FW_PRINTENV:-/etc/fw_printenv}
fw_setenv=${STOCK_FW_SETENV:-/etc/fw_setenv}
usb_root=${STOCK_USB_ROOT:-/media}
expected_md5=${STOCK_MD5:-2fa5c763a49e565b963ec0cf87704762}
expected_size=${STOCK_SIZE:-80371736}

check() {
    [ -f "$image" ] || { echo 'stock image missing'; return 1; }
    [ "$(wc -c < "$image" | tr -d ' ')" = "$expected_size" ] || { echo 'stock image size differs'; return 1; }
    [ "$($fw_printenv -n EMMCUpgradeEnable 2>/dev/null)" = 0 ] || { echo 'an internal update is already armed'; return 1; }
    [ -f "$live" ] || { echo 'printer status unavailable'; return 1; }
    # The printer's BusyBox has no stat applet. live.json embeds its own wall clock.
    stamp=$(head -c 90 "$live" | sed -n 's/.*"now":\([0-9]*\)\..*/\1/p')
    case "$stamp" in ''|*[!0-9]*) echo 'printer status has no clock'; return 1 ;; esac
    now=$(date +%s)
    [ "$now" -ge "$stamp" ] && [ $((now - stamp)) -le 5 ] || { echo 'printer status is stale'; return 1; }
    grep -q '"job":{"active":0,' "$live" || { echo 'printer is printing'; return 1; }
    # "xm" (sgtilt.c) = printer state idle, no logger session, tilt homed and not moving.
    grep -q '"xm":{"pos":[-0-9.]*,"idle":1,"busy":0,' "$live" || { echo 'printer is busy or the tilt is not homed'; return 1; }
    # Nothing may heat while the app is down for the flash: heater state 0 and pre-heat off.
    grep -q '"heat":{[^}]*"state":0,"printing":0,"pre":0,' "$live" || { echo 'switch the resin heating off first'; return 1; }
    # USB updates run before sdstar. Refuse while any USB image can take priority.
    if find "$usb_root" -mindepth 2 -maxdepth 2 -name ChituUpgrade.bin ! -path "$image" | grep -q .; then
        echo 'remove the USB firmware stick first'; return 1
    fi
    # Last, the slow step (reads the whole image): the printer does not verify the data it flashes.
    set -- $(md5sum "$image")
    [ "$1" = "$expected_md5" ] || { echo 'stock image checksum differs'; return 1; }
    echo "$expected_md5"
}

case "${1:-}" in
    check) check ;;
    arm)
        msg=$(check) || { echo "$msg"; exit 1; }
        "$fw_setenv" EMMCUpgradeEnable 1
        [ "$($fw_printenv -n EMMCUpgradeEnable 2>/dev/null)" = 1 ] || {
            echo 'could not verify the update flag'; exit 1;
        }
        echo armed
        ;;
    disarm)
        "$fw_setenv" EMMCUpgradeEnable 0
        [ "$($fw_printenv -n EMMCUpgradeEnable 2>/dev/null)" = 0 ] || {
            echo 'could not clear the update flag'; exit 1;
        }
        echo disarmed
        ;;
    *) echo 'usage: stock_restore.sh check|arm|disarm' >&2; exit 2 ;;
esac
