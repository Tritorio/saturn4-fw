#!/usr/bin/env bash
# Cross-compile mods/src/sgtap/sgcam.c -> build/sgcam/sgcam (executable for the printer, glibc 2.28, see build_sgtap.sh)
set -euo pipefail
cd "$(dirname "$0")/.."
LIB=work/fs/rootfs/lib; CC=arm-linux-gnueabihf-gcc; OUT=build/sgcam; mkdir -p "$OUT"
CRT="$(dirname "$($CC -print-file-name=crt1.o)")"
$CC -std=gnu11 -U_TIME_BITS -U_FILE_OFFSET_BITS -O2 -Wall -Wextra -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-stack-protector \
    -nostdlib -o "$OUT/sgcam" "$CRT/crt1.o" "$CRT/crti.o" mods/src/sgtap/sgcam.c "$LIB/libc.so.6" "$CRT/libc_nonshared.a" -lgcc "$CRT/crtn.o" \
    -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3 -Wl,-z,defs
bad=$(arm-linux-gnueabihf-objdump -T "$OUT/sgcam" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | awk -F_ '{split($2,v,"."); if (v[1]>2 || (v[1]==2 && v[2]>28)) print}')
[ -z "$bad" ] || { echo "newer glibc symbols: $bad"; exit 1; }
arm-linux-gnueabihf-strip -o "$OUT/sgcam.stripped" "$OUT/sgcam"
ls -l "$OUT"; md5sum "$OUT/sgcam.stripped"
