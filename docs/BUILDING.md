# Building and flashing

## 1. Tools
Linux or WSL2. `tools/setup_env.sh --check` reports what is missing, `tools/setup_env.sh --all` installs it:
`python3`, `xxd`, `xz-utils`, `squashfs-tools`, `mtd-utils` (`mkfs.ubifs`, `ubinize`), `u-boot-tools`,
`fakeroot`, `binwalk`, `binutils`, the ARM cross compiler `gcc-arm-linux-gnueabihf`, and a Python venv
(`.venv`) with `ubi_reader` and `capstone`. `Pillow` (system Python) is only needed for the manual tools.
For the manual screenshots and dashboard tests you also need headless Chrome.

## 2. The stock firmware (not in this repository)
Download the official **V1.5.9** package for the Saturn 4 Ultra 16K from Elegoo. The file used here is
`Saturn4Ultra_OTA/ChituUpgrade.bin`, md5 `2fa5c763a49e565b963ec0cf87704762` (all checksums:
`docs/stock_checksums.txt`). Place it at that path. **A different version will not work**: the library checks
the machine code of V1.5.9 and the tools verify the container layout.

```bash
.venv/bin/python tools/unpack.py Saturn4Ultra_OTA/ChituUpgrade.bin work --extract-fs
```
This creates `work/manifest.json`, `work/raw/*.bin` and `work/fs/<volume>/` (the stock `customer` volume
and `rootfs`, whose `lib/` is the glibc 2.28 the library is linked against).
Sanity check (optional): `python3 tools/pack.py work out/check.bin` must reproduce the stock md5.

## 3. Password
```bash
tools/mkshadow.sh      # asks for a root password, writes mods/customer/mod/shadow (git-ignored)
```
This is the telnet root password **and** the dashboard login. A flash always installs the password of the
image, so build every image with the password you want.

## 4. Library only (fast loop)
```bash
AA=1 tools/build_sgtap.sh        # -> build/sgtap/libsgtap.so and libsgtap.stripped.so (installed on the printer)
```
Zero compiler warnings is the rule. The script also checks that no symbol needs a glibc newer than 2.28.

## 5. Image
```bash
VARIANT=normal tools/build_mod.sh <number>_<name>_normal
```
* `VARIANT=normal` (everyday line, includes Multi Exposure) or `aa` (same code plus the experimental display tools).
* Steps: copy the stock customer volume + the overlay `mods/customer/`, add the library, `mkfs.ubifs` with the stock
  parameters, pack with `--only customer`, unpack the result again and diff it against what was built.
* Output: `out/<name>/ChituUpgrade.bin` and `out/<name>/MD5SUMS`.
* Optional switches: `RELEASE=1` (uses `mods/release/shadow`, a password file you create, instead of the dev password),
  `CPU_PATCH=0` (leave the stock busy loops of the UI thread), `CPU_UI_MS=<ms>`.
* The image header shows `build <number> · mod <version>` taken from the name and `mods/customer/mod/VERSION`.

## 6. Flash
1. FAT32 USB stick with **no** `ChituUpgrade.bin` on it yet; copy the new one to its root.
2. **Compare the md5 of the file on the stick** with `out/<name>/MD5SUMS` (`md5sum` on Linux, `Get-FileHash` on
   Windows). The printer does not verify the data.
3. Printer off → insert the stick → power on → wait for the automatic restart → remove the stick.
4. Open `http://<printer-ip>/` and log in with the password from step 3.

## 7. Recovery
* Flash the official stock `ChituUpgrade.bin` the same way (keep your downloaded copy!).
* While the dashboard still works: Settings → Return to stock firmware (verifies the cached stock image, arms
  U-Boot's own updater; needs an idle, homed printer, heating off, no stick; **never cut the power while the
  update bar is shown**; then power-cycle with the switch).
* Without the app: `touch /customer/mod/sgtap_disable` over telnet and reboot disables the library.

## Troubleshooting
* `work/fs/rootfs/lib` missing → run the unpack with `--extract-fs`.
* `mods/customer/mod/shadow missing` → run `tools/mkshadow.sh`.
* `kernel 4.9: UBIFS fmt_version` errors → do not change `tools/build_volume.py`; it refuses wrong parameters.
* After flashing, the dashboard shows the old page → the browser cached it; hard reload.
