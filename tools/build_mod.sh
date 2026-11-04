#!/usr/bin/env bash
# Build a customer-only firmware image = stock customer volume + overlay mods/customer/.
#
# Usage: VARIANT=normal|aa tools/build_mod.sh NAME     e.g. VARIANT=normal tools/build_mod.sh 38_mod_8.30_normal
#   VARIANT=normal: libsgtap WITH the board link code since build 44 (Multi Exposure needs it; MULTIEXPO=0 builds the old stand-in version without -DSGTAP_AA)
#   VARIANT=aa:     "anti-aliasing" variant = normal + sgspi.c (tracer) + sgdisp.c (lcdshow / lcdswap / lcdoff, fan, UV)
#   (VARIANT has no default on purpose; put the variant in NAME, e.g. ..._normal / ..._AA)
#   RELEASE=1 tools/build_mod.sh NAME    -> public release: the dev root password file is NOT used; mods/release/shadow (the generic password of the
#                                           release, created by the user) is installed as mod/shadow instead; default 0 = dev image
#   CPU_PATCH=0 tools/build_mod.sh NAME  -> stock chitu (default 1: tools/patch_cpu.py, 8.21)
#   CPU_UI_MS=5 tools/build_mod.sh NAME  -> UI loop sleep in the patch (default 50 ms; 28 = 5, 29 = 10)
#   -> out/NAME/ChituUpgrade.bin + out/NAME/MD5SUMS
#
# Steps: copy work/fs/customer under fakeroot (ownership kept), copy the overlay on top
# (new files root:root), mkfs.ubifs with stock parameters, pack with --only customer,
# then unpack the result again and check that the tree inside the image is exactly the
# tree we built, and list what differs from stock.
set -euo pipefail
[ $# -eq 1 ] || { sed -n '2,9p' "$0"; exit 2; }
NAME="$1"
cd "$(dirname "$0")/.."
PY=.venv/bin/python
WORK=work
B="build/mod/$NAME"
OUT="out/$NAME/ChituUpgrade.bin"
VARIANT="${VARIANT:-}"
[ "$VARIANT" = normal ] || [ "$VARIANT" = aa ] || { echo "set VARIANT=normal or VARIANT=aa"; exit 2; }
CPU_PATCH="${CPU_PATCH:-1}"
[ "$CPU_PATCH" = 0 ] || [ "$CPU_PATCH" = 1 ] || { echo "CPU_PATCH must be 0 or 1"; exit 2; }
CPU_UI_MS="${CPU_UI_MS:-50}"
RELEASE="${RELEASE:-0}"
[ "$RELEASE" = 0 ] || [ "$RELEASE" = 1 ] || { echo "RELEASE must be 0 or 1"; exit 2; }

[ -d "$WORK/fs/customer" ] || { echo "run: $PY tools/unpack.py Saturn4Ultra_OTA/ChituUpgrade.bin work --extract-fs"; exit 1; }
[ "$RELEASE" = 1 ] || [ -f mods/customer/mod/shadow ] || { echo "mods/customer/mod/shadow missing: run tools/mkshadow.sh"; exit 1; }
[ "$RELEASE" = 0 ] || [ -f mods/release/shadow ] || { echo "mods/release/shadow missing (the generic root password file of the public release)"; exit 1; }

rm -rf "$B"; mkdir -p "$B"
# 8.3: cross-compile the serial tap, installed as mod/libsgtap.so
EXTRA="$(realpath "$B")/extra"; mkdir -p "$EXTRA/mod"
# build number + date, shown on the dashboard next to the printer's name
printf '%s\n%s\n' "$NAME" "$(date -u '+%Y-%m-%d %H:%M UTC')" > "$EXTRA/mod/BUILD"
[ "$RELEASE" = 0 ] || { cp mods/release/shadow "$EXTRA/mod/shadow"; echo "public release: generic root password" > "$EXTRA/mod/RELEASE"; }
if [ -d mods/src/sgtap ]; then
    # Multi Exposure (sgmx.c: it is not Anti-Aliasing) needs the board link code of sgspi.c / sgdisp.c: the normal line carries it too (MULTIEXPO=0 = without, stand-ins of sgnoaa.c)
    if [ "$VARIANT" = aa ] || [ "${MULTIEXPO:-1}" = 1 ]; then AA=1 tools/build_sgtap.sh > "$B/build_sgtap.log"; else AA=0 tools/build_sgtap.sh > "$B/build_sgtap.log"; fi
    cp build/sgtap/libsgtap.stripped.so "$EXTRA/mod/libsgtap.so"
fi
# camera relay (sgcam.c, the dashboard's Camera card): a plain executable, started by hook.sh
tools/build_sgcam.sh > "$B/build_sgcam.log"
cp build/sgcam/sgcam.stripped "$EXTRA/mod/sgcam"
chmod 755 "$EXTRA/mod/sgcam"
STATE="$(realpath "$B")/customer.fakeroot"
# copy stock tree + overlay inside one fakeroot session -> state file with root owners
fakeroot -i "$WORK/fs/customer.fakeroot" -s "$STATE" -- sh -euc '
    cp -a "$1/fs/customer" "$2/customer"
    cp -r --no-preserve=ownership mods/customer/. "$2/customer/"
    cp -r --no-preserve=ownership "$3/." "$2/customer/"
    chown -R 0:0 "$2/customer/mod" "$2/customer/app0.sh"
    chmod 755 "$2/customer/app0.sh" "$2/customer/mod"
    find "$2/customer/mod" -type f -exec chmod 644 {} +
    find "$2/customer/mod" -type f -name "*.sh" -exec chmod 755 {} +
    chmod 755 "$2/customer/mod/sgcam"   # executable program started by hook.sh ([ -x ] check); the chmod 644 above would drop it (build 47 bug)
    chmod 600 "$2/customer/mod/shadow"
    find "$2/customer" -type f -name "*:Zone.Identifier" -delete
    # 8.21: pace the stock busy loops (refuses anything but the stock V1.5.9 chitu)
    if [ "$4" = 1 ]; then
        python3 tools/patch_cpu.py --ui-ms "$5" "$2/customer/resources/chitu" "$2/customer/resources/chitu"
    fi
    # UBIFS keeps whole seconds only: round the new timestamps so the verify is exact
    T="@$(date +%s)"
    find "$2/customer/mod" "$2/customer/app0.sh" "$2/customer" -maxdepth 0 -exec touch -h -d "$T" {} +
    find "$2/customer/mod" -exec touch -h -d "$T" {} +
    (cd mods/customer && find . -type f ! -name "*:Zone.Identifier") | while read -r p; do touch -h -d "$T" "$2/customer/$p"; done
    if [ "$4" = 1 ]; then touch -h -d "$T" "$2/customer/resources/chitu"; fi
' sh "$WORK" "$B" "$EXTRA" "$CPU_PATCH" "$CPU_UI_MS" "$RELEASE"

python3 tools/build_volume.py "$WORK" customer "$B/customer.ubifs" --root "$B/customer" --state "$STATE"
mkdir -p "out/$NAME"
python3 tools/pack.py "$WORK" "$OUT" --only customer --replace customer="$B/customer.ubifs"

echo; echo "== verify: unpack the new image and compare with the built tree"
$PY tools/unpack.py "$OUT" "$B/verify" --extract-fs > "$B/verify.log"
tools/compare_fs.sh "$B/customer" "$STATE" "$B/verify/fs/customer" "$B/verify/fs/customer.fakeroot"

echo; echo "== changes vs stock (expected: app0.sh + mod/, + resources/chitu with CPU_PATCH=1)"
tools/compare_fs.sh "$WORK/fs/customer" "$WORK/fs/customer.fakeroot" \
    "$B/verify/fs/customer" "$B/verify/fs/customer.fakeroot" || true

(cd "out/$NAME" && md5sum ChituUpgrade.bin > MD5SUMS)
(cd out && md5sum */ChituUpgrade.bin > MD5SUMS)
echo; cat "out/$NAME/MD5SUMS"
