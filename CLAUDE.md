# saturn4-fw – guide for AI assistants (Claude Code, Codex, Cursor, ...)

Read this first. It is short on purpose; the details are in `docs/ARCHITECTURE.md`,
`docs/BUILDING.md` and the reverse-engineering notes `docs/*.txt` (how each function of the app was found out
and measured). Search them before re-deriving anything ("grep -n" for a keyword, then read around the hit).

## What this project is
A customer-volume-only modification of the stock firmware **V1.5.9** of the Elegoo Saturn 4 Ultra
16K, installed by USB update. Pieces: `libsgtap.so` (C, `LD_PRELOAD` into the stock app `chitu`),
a dashboard page (`mods/customer/mod/www/index.html`, one file, no dependencies), a helper
service (`sgwebd.tcl`, Jim Tcl), shell hooks, and the Python/bash tools that unpack, rebuild and
pack the update image.

## Hard constraints (never break these)
1. **Only the `customer` UBI volume is modified.** Never modify or rewrite CIS, IPL, IPL_CUST,
   U-Boot, the partition layout or the U-Boot environment layout; keep kernel and rootfs stock
   (the kernel size is hard-coded in `bootcmd`). `tools/pack.py` refuses to replace those chunks;
   do not weaken that.
2. **The only way into the printer and the only recovery is the USB firmware update.** Never
   suggest opening the printer, UART, JTAG or NAND dumps.
3. **The printer does not verify update data** (U-Boot `crccheck` is a no-op): always compare the
   md5 of the file *on the USB stick* with `out/<n>/MD5SUMS` before flashing.
4. **No `fw_setenv`** anywhere, except `mods/customer/mod/stock_restore.sh`, which may set
   `EMMCUpgradeEnable` (and only that) for the "Return to stock firmware" button.
5. **Never write passwords into any file, note, commit or chat log.** The root/dashboard password
   is created by `tools/mkshadow.sh` into `mods/customer/mod/shadow` (git-ignored). Ask the user
   for the telnet password each session; it only goes into the `TPW=` environment variable of one
   command.
6. **Never commit Elegoo files** (`ChituUpgrade.bin`, `ChituFPGA.bin`, extracted rootfs/kernel,
   UI images, the user-manual PDF). `work/`, `out/`, `build/`, `backups/`, `Saturn4Ultra_OTA/` are
   git-ignored for that reason.
7. Anything that touches a real printer needs the owner's explicit go: say what you will do,
   back up what you replace, do the smallest change, verify afterwards.

## Repository map
```
mods/src/sgtap/      libsgtap C sources (sgtap.c includes the others; host/pty tests next to them)
mods/customer/       overlay copied into /customer: app0.sh, mod/hook.sh, mod/sgwebd.tcl, mod/www/index.html,
                     machlib.tcl + machguard.tcl (machine settings file), stock_restore.sh, VERSION
tools/               unpack.py / pack.py / build_volume.py / build_mod.sh / build_sgtap.sh, tsh.py (telnet runner),
                     re/ (capstone xref helpers), analysis/ (force / wait / tilt analysis), manual + screenshot tools
docs/                ARCHITECTURE.md, BUILDING.md, RE notes (*.txt), user_manual.html (+ manual_src.html)
board-report/        read-only report script for other board / firmware revisions
```

## Build and test loop (no printer needed for most work)
* One-time: `tools/setup_env.sh --all`; unpack the stock firmware (`docs/BUILDING.md`). That creates
  `work/` (needed: `work/fs/rootfs/lib` is the glibc 2.28 the library is linked against).
* Library: `AA=1 tools/build_sgtap.sh` → `build/sgtap/libsgtap.stripped.so`. Warnings must stay at zero.
  `AA=1` compiles the board-link code (`sgspi.c`, `sgdisp.c`, `sgmx.c`); without it `sgnoaa.c`
  stand-ins are used. The "normal" image line is built with the board-link code too (Multi Exposure).
* Image: `tools/mkshadow.sh` once, then `VARIANT=normal tools/build_mod.sh <n>_<name>_normal`
  → `out/<n>_<name>_normal/ChituUpgrade.bin` + `MD5SUMS` (the script re-unpacks the result and diffs it).
  Number builds consecutively and say in the notes which files an image contains.
* Host tests (plain gcc): `gcc -o /tmp/sgdltest mods/src/sgtap/sgdltest.c && /tmp/sgdltest`,
  `sgmxtest.c` (see its header; `tools/analysis/mx_mask_check.py` checks its output).
  `sgtest.c` / `sglogtest.c` run on the printer from `/tmp` with a pty pair (headers explain).
* Dashboard page: no build step. Test it with headless Chrome against a copy of the page
  (the page redirects any port other than 3030: strip that `location.replace` line in the copy),
  a tiny `python3 -m http.server` that serves a mock `/tmp/sgweb/live.json`, and
  `--screenshot` / `--dump-dom --enable-logging=stderr` to catch console errors. Check **duplicate
  element ids** (`re.findall(r'\sid="..."')`) before every upload: a duplicate id once broke a card.
  `tools/manual_shots.py` shows how to stub commands and fake data for screenshots.
* Manual: edit `docs/manual_src.html`, run `python3 tools/make_manual.py` (needs Pillow),
  screenshots with `tools/manual_shots.py` (needs a running printer's live data + chrome).

## Talking to a running printer (only with the owner's go)
`TPW=<password> tools/tsh.py <ip> '<command>'` runs a command as root over telnet (BusyBox: no
`timeout`, no `pkill`, `tar -C` applies to the whole archive). `TPUT=local=remote,...` uploads small
files first (gzip+base64, ~10 s per 100 KB). Deploy a live change like this:
1. back up the file to `/media/mmcblk0p2/sglog/live_backup_<date>/`;
2. upload to `<name>.new`, compare md5, then `mv <name>.new <name>` (atomic);
3. the dashboard page and `sgwebd.tcl` act immediately (restart sgwebd by killing its pid; the hook respawns it);
   **a new `libsgtap.so` is only loaded after a POWER cycle** (a soft reboot gives a false strain-gauge
   self-check error). Kill switches: `touch /customer/mod/<name>_disable` (+ restart), see `docs/ARCHITECTURE.md`.

## Rules for the C library (`mods/src/sgtap`)
* Cross compile only with `tools/build_sgtap.sh`: links against the printer's own glibc 2.28, `-nostdlib`,
  `-Wl,-z,defs`, `-U_TIME_BITS -U_FILE_OFFSET_BITS`, `-std=gnu11`. Do **not** use `strtol`, `strtoull`, `atoi`,
  `sscanf`, `scanf` (GCC 15 maps them to `__isoc23_*`, missing in glibc 2.28): the script's symbol check fails.
* The app **daemonizes (fork + setsid) and runs commands with vfork**: preload code must be fork-safe
  (atfork handler) and vfork-safe. **Threads must be started from the writer tick, once per process** –
  a thread created at init dies in the fork.
* Everything that reads or writes the app's memory is addressed by the fixed V1.5.9 addresses and
  guarded by a code-word signature check (`*_sig[]` tables) plus a kill-switch file. Add a signature
  for every new patch point, store aligned words, keep the written value inside the app's own limits.
* Facts that cost time to find: the print struct `P = *(0x2eb6d8)` **stays after a job** until the
  finished screen is dismissed (use the logger's session, not `P == 0`, as "job over"); `P+0x248` is a
  float; plan per-layer work at the **start of the exposure** (waits can be 50 ms: a 20 Hz watcher misses
  them); layer numbers are 0-based in the app, 1-based on the dashboard.

## Rules for the dashboard (`index.html`)
* One self-contained file (CSS variables for the dark/light theme, no external requests). `live.json`
  (written by libsgtap every 0.5 s) is the data feed; writes go to `sgwebd.tcl` (:8080) as `cmd` lines that
  libsgtap parses – every new command needs a whitelist entry in `sgwebd.tcl` **and** the parser.
* Controls that write must be greyed while logged out: add them to `WRITE_SEL` / `WRITE_CARDS`.
* Explanations hide behind a "?" (`.qbtn` + `.qnote`); On/Off cards use the `CXD` table; the work area is a
  two-column masonry (`masSpan`). Look at an existing card before writing a new one.

## Conventions
* Notes are plain `.txt` in `docs/` (new findings: a new file or a section of the matching note). Bump the version
  in `mods/customer/mod/VERSION` (`saturn4-fw mod <x.y> ...`, shown in the dashboard header) when a build is made.
* Python tools: standard library only (except `ubi_reader`, `capstone`, `Pillow`).
* Keep answers and docs short and in English. Prefer small reversible changes; verify with the printer's own
  log (`/media/mmcblk0p2/log`, `log1`/`log2`) when a behaviour is in doubt.
