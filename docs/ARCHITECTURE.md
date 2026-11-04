# Architecture

Everything below is valid **only for stock firmware V1.5.9** (the app `chitu`, ARM EABI5 hard-float,
stripped, ~2.9 MB). Addresses are virtual addresses of that binary (text base 0x10000 = file offset 0).
The reverse-engineering notes in `docs/*.txt` hold the evidence for each fact.

## The printer in one page
* SoC SigmaStar SSC011A (Infinity2M), Linux 4.9, BusyBox. Storage: NAND with UBI volumes `rootfs`,
  `miservice` (/config), `customer` (/customer), `appconfigs`, `parameter`; eMMC `mmcblk0p1` (machine data:
  `machine_param_ref.bin`, `ui_param_ref.bin`, print history, thumbnails, Wi-Fi), `p2` (app logs `log1`/`log2`,
  `sglog/` = everything this mod writes), `p3` (print files).
* The app starts from `/customer/app0.sh`. The mod's `app0.sh` runs the boot guard, starts
  `chitu` with `LD_PRELOAD=/customer/mod/libsgtap.so` (unless `sgtap_disable` exists), then `mod/hook.sh`.
* Ports: 23 telnet (root, password from `mod/shadow`), 80 redirect to the dashboard, 3030 the app's own web
  server (serves the whole file system read-only, SDCP v3 websocket for status / print control / uploads),
  8080 `sgwebd` (all writes of the mod), 554 RTSP camera.
* The strain-gauge board talks to the SoC over a UART (`ttyS1`); the app polls it ~5 times per second. The
  display board (LCD + UV) is driven over SPI, the tilt (X) motor is a TMC5130-like driver on SPI, Z is a
  stepper (1/1600 mm per step).

## Update container and what is replaced
`ChituUpgrade.bin` is a SigmaStar USB-upgrade container: a U-Boot script (<4 KB) plus chunks (payload + 8
hex chars crc32). `tools/unpack.py` splits and verifies it, `tools/pack.py` repacks it bit-identically
(`pack(unpack(stock))` = the stock md5). The mod replaces **only the `customer` UBIFS volume**, built by
`tools/build_volume.py` with the original superblock parameters (LEB 126976, min I/O 2048, fmt_version 4:
kernel 4.9 – never format 5).

## libsgtap (`mods/src/sgtap`)
`sgtap.c` is the entry: it wraps libc calls of the app (open / read / write / termios / fork) and `#include`s
the other files into one translation unit (static, no headers).

| File | Responsibility |
|---|---|
| `sgtap.c` | LD_PRELOAD hooks, serial tap of the gauge line, fork / vfork safety, build switches |
| `sglog.c` | per-print CSV logger, app-log follower (print phases, sessions, alerts) |
| `sgweb.c` | `live.json` feed (every 0.5 s), command intake from `sgwebd`, tare / auto-tare / k |
| `sgheat.c` | resin heating (reads/writes the app's heater setting), plate move into the resin, Z axis access |
| `sgwait.c` | dynamic wait, wait & heat, tare in the wait, settings snapshot, the 20 Hz wait thread |
| `sgexpo.c` | exposure time override per layer class (also armed before a job) |
| `sgtilt.c`, `sgdl.c` | tilt speed profile, two-stage fix, Dynamic lift controller + passive peel watch, slow layers, X move; `sgdl.c` is pure logic (host-tested) |
| `sgzmon.c` | Z monitor: print height per layer from the Z step counter and the app's Z targets; `LEVELS.csv` (Z zero found by each job's levelling) |
| `sgmach.c` | machine settings file (live struct + app saver), Flex Plate set |
| `sgsys.c` | CPU / RAM / storage meters |
| `sgspi.c`, `sgdisp.c`, `sgmx.c`, `sgmxmask.h` | display-board link (SPI tap), LCD image / fan / UV control, **Multi Exposure** (masking the layer stream per quadrant). `sgnoaa.c` holds stand-ins when built without `-DSGTAP_AA` |

### How a patch is made safe
1. A `*_sig[]` table lists code words at fixed addresses (for example `{0x7d17c, 0xeb01570b}`); the module
   switches itself off unless every word matches the running binary.
2. Values are written with aligned word/float stores into the app's own structures; the app's own next pass
   uses them (nothing is blocked, no app lock is taken, a wait can only get shorter).
3. Every module has a kill-switch file (below) and a safe default (a failed check = do nothing).

### Key structures (V1.5.9)
* Print struct `P = *(uint32_t *)0x2eb6d8` (heap; **stays after a job** until the finished screen is dismissed):
  `+0x2ac` layer (0-based), `+0x30b` state (2 = wait after retract), `+0x30c` exposing flag, `+0x318` wait start
  (ms, CLOCK_MONOTONIC_RAW), `+0x338` exposure start, `+0x24c` exposure time (float ms), `+0x248` UV PWM (float),
  `+0x25c` wait after retract (float ms), `+0x220` / `+0x228` target Z and thickness of the current layer
  (float mm), `+0x110` / `+0x114` bottom / transition layer counts, `+0x37c` skip mask (bit 4 = level).
* Machine settings struct at `0x39fa30` (floats / doubles; Z zero at `+0x40`, I204 / I205 resin and levelling
  values), tilt profile at `0x39fb40`. Z axis struct `*(0x2fa124)`: `+0x18` mm/step, `+0x38` zero copy
  (refreshed only at a restart), `+0x50` position in steps; tilt axis `*(0x2fa124 + 4)`.
* Printer state word `0x30280c` (10 = printing; stays 10 until the print is acknowledged on the screen).

## Dashboard and helper
* `mods/customer/mod/www/index.html` is served by the app's web server. It polls `/tmp/sgweb/live.json`, talks
  SDCP v3 over the websocket on :3030 (status, print control, upload) and posts commands to `sgwebd` on :8080.
* `sgwebd.tcl` (Jim Tcl) validates commands against a whitelist, keeps login sessions in memory (token header
  `X-SG-Token`, password = the root password), writes settings files and hands the commands to the library.
  `machlib.tcl` / `machguard.tcl` read, write and guard `machine_param_ref.bin` (22/23-byte header + JSON + CRC).
* `stock_restore.sh` arms U-Boot's own recovery from the cached stock image (`EMMCUpgradeEnable`).

## Settings and data files (`/media/mmcblk0p2/sglog/`)
See the table at the end of `docs/user_manual.html` (WAIT, SETTINGS, profiles/, K, TILTCUSTOM, DYNLIFT, GSYNC,
MULTIEXPO, layertilt/, UISLEEP, machine_backups/, LEVELS.csv, the job CSVs ...). Live backups of replaced files go
to `live_backup_<date>/` by convention.

## Kill switches
`touch /customer/mod/<name>` then restart / power-cycle: `sgtap_disable` (whole library), `sgwebd_disable`,
`sgheat_disable`, `sgwait_disable`, `sgexpo_disable`, `sgzmon_disable`, `sgmx_disable`, `sgtilt_disable`,
`sgmach_disable`, `machguard_disable`, `sguisleep_disable`. Job logging off: `touch /media/mmcblk0p2/sglog/DISABLE`.
