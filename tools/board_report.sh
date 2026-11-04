#!/bin/sh
# board_report.sh - READ-ONLY identification of a Saturn 4 Ultra 16K board + firmware, to compare units.
# Runs on the printer (busybox sh). Writes nothing, changes nothing, no personal data (MAC addresses are masked,
# no Wi-Fi data, no serial numbers, no print file names).
#
# Compare the checksums with the values printed in the report: if the stock files differ, tell us BEFORE flashing anything.
# From a PC with telnet access:   TPW=<password> TPUT=tools/board_report.sh=/tmp/board_report.sh tools/tsh.py <printer ip> 'sh /tmp/board_report.sh'
# On the printer itself:          sh board_report.sh > /tmp/board_report.txt   (then copy the text)
mask() { sed -E 's/([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/XX:XX:XX:XX:XX:XX/g'; }
sec() { echo; echo "== $1"; }

echo "BOARD REPORT  $(date -u '+%Y-%m-%d %H:%M UTC')"

sec "kernel / cpu / memory"
uname -srmv
cat /proc/cmdline | mask
grep -E '^(processor|model name|Hardware|CPU implementer|CPU part|CPU revision|Features)' /proc/cpuinfo
grep -E '^(MemTotal|MemFree)' /proc/meminfo
cat /proc/device-tree/model 2>/dev/null; echo
cat /proc/device-tree/compatible 2>/dev/null | tr '\0' ' '; echo

sec "flash layout"
echo "-- /proc/mtd"; cat /proc/mtd 2>/dev/null
echo "-- /proc/partitions"; cat /proc/partitions
echo "-- ubi volumes (name, bytes)"
for v in /sys/class/ubi/ubi*_*; do [ -r "$v/name" ] && echo "$(basename $v) $(cat $v/name) $(cat $v/data_bytes 2>/dev/null)"; done
echo "-- mounts"; grep -E ' (ubi|ubifs|squashfs|jffs2|vfat|exfat|ext4) ' /proc/mounts | mask

sec "eMMC (no serial number)"
for f in name manfid oemid date fwrev hwrev prv type; do echo "$f=$(cat /sys/block/mmcblk0/device/$f 2>/dev/null)"; done
echo "size_sectors=$(cat /sys/block/mmcblk0/size 2>/dev/null)"

sec "U-Boot environment (selected, read only)"
FWP=""
for p in fw_printenv /etc/fw_printenv /sbin/fw_printenv /usr/sbin/fw_printenv /bin/fw_printenv /usr/bin/fw_printenv; do command -v $p >/dev/null 2>&1 && { FWP=$p; break; }; done
if [ -n "$FWP" ]; then $FWP cbd_board_name dispout chitu_factory_mode CurrentVersion UpgradeImage EMMCUpgradeEnable usb_folder bootcmd 2>&1 | mask; else echo "fw_printenv not found"; fi

sec "software checksums"
echo "(stock V1.5.9 on the project printer: chitu 116dfee541b33b913a2fd898b0c7c0f4, ChituUpgrade.bin 2fa5c763..., ChituFPGA.bin 36ac00b6..., gauge board image b6979fd4...; a printer with the saturn4-fw mod shows the patched chitu 629a8c47...)"
md5sum /customer/resources/chitu /customer/resources/upgrade_cbd_app_lcd_strain_gauge_*.bin /media/mmcblk0p1/ChituFPGA.bin /media/mmcblk0p1/ChituUpgrade.bin /customer/app0.sh 2>/dev/null | sed 's/  / /'
ls -l /customer/resources/chitu | awk '{print "chitu size", $5}'
strings /customer/resources/chitu 2>/dev/null | grep -E '^V[0-9]+\.[0-9]+\.[0-9]+' | head -3
[ -f /customer/mod/BUILD ] && { echo "-- mod present:"; cat /customer/mod/BUILD; head -c 60 /customer/mod/VERSION; echo; }

sec "Wi-Fi / USB hardware (the stock kernel carries drivers for several Wi-Fi chips: this shows which one is used)"
echo "-- loaded modules"; lsmod 2>/dev/null | awk '{print $1}' | tr '\n' ' '; echo
echo "-- network interfaces"; ls /sys/class/net 2>/dev/null | tr '\n' ' '; echo
echo "-- usb devices (vendor:product, name)"
for d in /sys/bus/usb/devices/*; do [ -r "$d/idVendor" ] && echo "$(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/product 2>/dev/null)"; done

sec "app log: version / FPGA / gauge lines (first 25; the SDCP request lines and the printer ID are left out)"
for f in /media/mmcblk0p2/log1 /media/mmcblk0p2/log2; do
  [ -r "$f" ] && grep -i -E 'version|fpga|hardware|hw_|ceb-iap|strain|tmc|firmware' "$f" 2>/dev/null | grep -v -E 'sdcp|MainboardID|RequestID|wlan|ssid|psk|file_name|file_manager' | mask | head -25 && break
done

sec "kernel messages (first 50 lines)"
dmesg 2>/dev/null | head -50 | mask

sec "device nodes of interest"
ls /dev | grep -E '^(ttyS|spidev|i2c|pwm|gpiochip|mmcblk0$|mtd[0-9]+$|fb|video)' | tr '\n' ' '; echo
echo
echo "END OF REPORT"
