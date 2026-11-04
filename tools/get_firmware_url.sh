#!/bin/bash
# get_firmware_url.sh - ask Elegoo's update server (the same request the printer sends) for the
# firmware package of a given machine ID and print the download link.
#
# Usage: tools/get_firmware_url.sh [MACHINE_ID] [-t FWTYPE] [-v VERSION] [-m MODEL] [-d]
#   MACHINE_ID  the printer's MainboardID (16 hex digits; SDCP "MainboardID", also in the app log).
#               If omitted, the script asks for it.
#   -t FWTYPE   1 = system firmware (default), 2 = AIC (Wi-Fi chip) part
#   -v VERSION  version REPORTED to the server (default 00.00.00 = "very old", so the server
#               offers the newest package; the real printer sends e.g. 01.05.09 for V1.5.9)
#   -m MODEL    machineType (default "ELEGOO Saturn 4 Ultra 16K")
#   -d          also download the package (curl -O) and print its md5
# Needs only bash + curl. Read-only request; sends nothing but the query below.
set -u
MID=""; FT=1; VER="00.00.00"; MODEL="ELEGOO Saturn 4 Ultra 16K"; DL=0
while [ $# -gt 0 ]; do
    case "$1" in
        -t) FT="$2"; shift 2 ;;
        -v) VER="$2"; shift 2 ;;
        -m) MODEL="$2"; shift 2 ;;
        -d) DL=1; shift ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) MID="$1"; shift ;;
    esac
done
command -v curl >/dev/null || { echo "curl not found" >&2; exit 1; }
if [ -z "$MID" ]; then
    read -r -p "Machine ID (16 hex digits): " MID
fi
MID=$(printf '%s' "$MID" | tr -d ' \r\n\t')
case "$MID" in
    *[!0-9a-fA-F]*|"") echo "invalid machine ID (expected hex digits only)" >&2; exit 1 ;;
esac

ENC_MODEL=${MODEL// /%20}
# the printer's own URL ends in "getInfo.do7" (sic)
URL="https://mms.chituiot.com/mainboardVersionUpdate/getInfo.do7?machineId=$MID&version=$VER&lan=en&firmwareType=$FT&machineType=$ENC_MODEL"
echo "request : $URL" >&2
R=$(curl -sS --max-time 20 -H "Accept-Language: en" "$URL") || { echo "request failed" >&2; exit 2; }
echo "response: $R" >&2

# tiny JSON field picker (string or number/bool/null values)
field() { printf '%s' "$R" | sed -n 's/.*"'"$1"'":\("\([^"]*\)"\|\([^,}]*\)\).*/\2\3/p'; }
UPD=$(field update); PKG=$(field packageUrl); HASH=$(field packageHash); NVER=$(field version)

if [ "$UPD" != "true" ] || [ -z "$PKG" ] || [ "$PKG" = "null" ]; then
    echo "no package offered (update=$UPD). Try a lower -v VERSION, or another -t / -m." >&2
    exit 3
fi
echo "version : $NVER"
echo "md5     : $HASH"
echo "url     : $PKG"
if [ "$DL" = 1 ]; then
    F=$(basename "${PKG%%\?*}")
    curl -L --fail -o "$F" "$PKG" && echo "saved   : $F  md5 $(md5sum "$F" | cut -d' ' -f1)"
fi
