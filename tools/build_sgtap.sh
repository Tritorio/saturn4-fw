#!/usr/bin/env bash
# Cross-compile mods/src/sgtap -> build/sgtap/{libsgtap.so,sgtest,sglogtest}, linked against the
# printer's own glibc 2.28 (work/fs/rootfs/lib) so no newer symbol versions sneak in.
# Ubuntu's armhf gcc defaults to _TIME_BITS=64/_FILE_OFFSET_BITS=64 (t64): undefine
# them, the target (glibc 2.28) and the chitu app use 32-bit time_t and off_t.
# GCC 15 defaults to C23, where glibc headers redirect strtol/atoi/... to
# __isoc23_* (glibc >= 2.38): use -std=gnu11. -z defs makes missing symbols fatal.
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=mods/src/sgtap; OUT=build/sgtap; LIB=work/fs/rootfs/lib
CC=arm-linux-gnueabihf-gcc
CFLAGS="-std=gnu11 -U_TIME_BITS -U_FILE_OFFSET_BITS -O2 -Wall -Wextra -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-stack-protector"
# AA=1: the "AA" variant with the board link tracer + LCD / fan / UV control (sgspi.c, sgdisp.c); default 0 = "normal" (stand-ins in sgnoaa.c).
# The test programs sgspitest / sgdisptest need AA=1.
[ "${AA:-0}" != 1 ] || CFLAGS="$CFLAGS -DSGTAP_AA"
mkdir -p "$OUT"
$CC $CFLAGS -Wno-format-truncation -fPIC -shared -nostdlib -Wl,-z,defs -o "$OUT/libsgtap.so" "$SRC/sgtap.c" \
    "$LIB/libc.so.6" "$LIB/libdl.so.2" "$LIB/libpthread.so.0" -lgcc
$CC $CFLAGS -nostdlib -o "$OUT/sgtest" \
    "$(dirname "$($CC -print-file-name=crt1.o)")"/{crt1.o,crti.o} "$SRC/sgtest.c" \
    "$LIB/libc.so.6" "$(dirname "$($CC -print-file-name=crt1.o)")/libc_nonshared.a" -lgcc \
    "$(dirname "$($CC -print-file-name=crt1.o)")/crtn.o" \
    -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3 2>/dev/null || echo "sgtest: build failed (test only)"
CRT="$(dirname "$($CC -print-file-name=crt1.o)")"
$CC $CFLAGS -nostdlib -o "$OUT/sglogtest" "$CRT/crt1.o" "$CRT/crti.o" "$SRC/sglogtest.c" \
    "$LIB/libc.so.6" "$CRT/libc_nonshared.a" -lgcc "$CRT/crtn.o" \
    -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3 2>/dev/null || echo "sglogtest: build failed (test only)"
$CC $CFLAGS -nostdlib -o "$OUT/sgspitest" "$CRT/crt1.o" "$CRT/crti.o" "$SRC/sgspitest.c" \
    "$LIB/libc.so.6" "$CRT/libc_nonshared.a" -lgcc "$CRT/crtn.o" \
    -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3 2>/dev/null || echo "sgspitest: build failed (test only)"
$CC $CFLAGS -nostdlib -o "$OUT/sgdisptest" "$CRT/crt1.o" "$CRT/crti.o" "$SRC/sgdisptest.c" \
    "$LIB/libc.so.6" "$CRT/libc_nonshared.a" -lgcc "$CRT/crtn.o" \
    -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3 2>/dev/null || echo "sgdisptest: build failed (test only)"
# every versioned symbol must exist in glibc 2.28
bad=$(arm-linux-gnueabihf-objdump -T "$OUT/libsgtap.so" | grep -oE 'GLIBC_[0-9.]+' | sort -uV |
      awk -F_ '{split($2,v,"."); if (v[1]>2 || (v[1]==2 && v[2]>28)) print}')
[ -z "$bad" ] || { echo "newer glibc symbols: $bad"; exit 1; }
# and no undefined symbol may lack a version (e.g. __isoc23_strtoull from C23 headers)
unver=$(arm-linux-gnueabihf-objdump -T "$OUT/libsgtap.so" | awk '$3=="*UND*" && $0 !~ /GLIBC_/ && $NF != "" {print $NF}' | grep -v -E '^(w|__gmon_start__|_ITM_|__cxa_finalize)' || true)
[ -z "$unver" ] || { echo "unversioned undefined symbols: $unver"; exit 1; }
arm-linux-gnueabihf-objdump -T "$OUT/libsgtap.so" | awk '/GLIBC_/{print $(NF-1), $NF}' | sort -u | tr '\n' ' '; echo
arm-linux-gnueabihf-strip -o "$OUT/libsgtap.stripped.so" "$OUT/libsgtap.so"
ls -l "$OUT"
