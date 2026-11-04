#!/usr/bin/env python3
"""Pace three verified busy loops in V1.5.9 chitu (customer volume only) and fix the resin-low resume.

Usage: python3 tools/patch_cpu.py [--ui-ms N] STOCK_CHITU OUTPUT_CHITU
The source must be the exact stock executable unpacked from our V1.5.9 OTA.
--ui-ms: the UI loop's sleep, 1-65 ms (default 50; image 28 used 5, image 29 10). libsgtap (8.22) replaces
it at run time with the dashboard's value: its usleep() wrapper recognises the call from the UI
stub by its return address UI_STUB + 8 = 0x2c14bc, so keep this layout.

Resume after the low-resin pause (build 49): the probe stage stops the job with wait code 11 ("the resin pos is abnormal low").
The suspend handler (0x783d4) does not know that code, logs "unknown wait b" and resumes into the PRINT state, so "force print"
skipped the levelling (plate to the stale zero: it hit the screen). The stub RESUME_STUB sends code 11 to the LEVEL state (P+0x5c,
r7 in that function: the same target as the handler's own "continue level") and leaves every other unknown code on the stock
target (P+0x8c). One instruction at 0x7889c (mov r1,r4) is replaced by a branch to the stub, which returns to 0x788a0.
"""
import hashlib
import os
import struct
import sys
from pathlib import Path

BASE = 0x10000  # executable PT_LOAD maps file offset zero here
STOCK_SHA256 = "e91bd45a4688c3435235214064d36be2c24e02dd6fd56c2532f0ed772333f318"
GAP_FILE = 0x2b14b4
GAP_VADDR = BASE + GAP_FILE
UI_STUB = GAP_VADDR
CAMERA_STUB = UI_STUB + 12
RESUME_STUB = UI_STUB + 24
NEW_LOAD_SIZE = GAP_FILE + 44


def branch(at, target, cond=0xE, link=False):
    delta = target - (at + 8)
    if delta % 4 or not -(1 << 25) <= delta < (1 << 25):
        raise ValueError("branch target out of range")
    return (cond << 28) | (0x0B000000 if link else 0x0A000000) | ((delta // 4) & 0xFFFFFF)


def movw_r0(value):
    if not 0 <= value <= 0xFFFF:
        raise ValueError("movw immediate out of range")
    return 0xE3000000 | ((value >> 12) << 16) | (value & 0xFFF)


def main():
    args = sys.argv[1:]
    ui_ms = 50
    if len(args) == 4 and args[0] == "--ui-ms":
        ui_ms = int(args[1])
        args = args[2:]
    if len(args) != 2 or not 1 <= ui_ms <= 65:
        sys.exit(__doc__)
    src, dst = map(Path, args)
    data = bytearray(src.read_bytes())
    digest = hashlib.sha256(data).hexdigest()
    if digest != STOCK_SHA256:
        sys.exit(f"unexpected chitu SHA256 {digest}; refusing CPU patch")
    if data[GAP_FILE:GAP_FILE + 44] != b"\0" * 44:
        sys.exit("executable file gap is not empty")
    # ELF32 program header 3 is the first R-X PT_LOAD. Extend it by 24 bytes
    # into its existing zero-filled file gap (44 bytes: UI, camera and resume stubs), still well before the next LOAD.
    ph = 52 + 3 * 32
    if struct.unpack_from("<8I", data, ph) != (
        1, 0, 0x10000, 0x10000, GAP_FILE, GAP_FILE, 5, 0x10000
    ):
        sys.exit("unexpected ELF executable program header")

    def put(addr, expected, words):
        off = addr - BASE
        old = data[off:off + len(expected) * 4]
        if old != struct.pack("<" + "I" * len(expected), *expected):
            sys.exit(f"unexpected instruction at {addr:#x}: {old.hex()}")
        data[off:off + len(words) * 4] = struct.pack("<" + "I" * len(words), *words)

    # Main: UI update; lv_timer_handler; branch back. Route that final branch
    # through a ui_ms usleep, then return to the top of the loop.
    put(0x188b4, [0xEAFFFFFC], [branch(0x188b4, UI_STUB)])
    put(UI_STUB, [0, 0, 0], [
        movw_r0(ui_ms * 1000), branch(UI_STUB + 4, 0x18598, link=True),
        branch(UI_STUB + 8, 0x188ac),
    ])
    # Camera scan: camera_flags bit 0 makes 0xd5480 jump back to lock/read
    # immediately. Add a 20 ms sleep only on that branch.
    put(0xd5480, [0x1AFFFFF8], [branch(0xd5480, CAMERA_STUB, cond=1)])
    put(CAMERA_STUB, [0, 0, 0], [
        movw_r0(20000), branch(CAMERA_STUB + 4, 0x18598, link=True),
        branch(CAMERA_STUB + 8, 0xd5468),
    ])
    # UDP 12300: poll(two fds, 0 ms) -> poll(two fds, 20 ms). The pipe remains
    # in the poll set, so queued outgoing work wakes it immediately.
    put(0xea428, [0xE3A02000], [0xE3A02014])
    # Resume after the low-resin pause: wait code 11 -> LEVEL state (see the docstring).
    put(0x7889c, [0xE1A01004], [branch(0x7889c, RESUME_STUB)])
    put(RESUME_STUB, [0, 0, 0, 0, 0], [
        0xE59422E8,                      # ldr r2, [r4, #0x2e8]   r4 = P + 0x8c here, so P + 0x374 = the wait code
        0xE352000B,                      # cmp r2, #0xb
        0x01A01007,                      # moveq r1, r7           LEVEL state (P + 0x5c)
        0x11A01004,                      # movne r1, r4           stock target
        branch(RESUME_STUB + 16, 0x788a0),
    ])
    struct.pack_into("<II", data, ph + 16, NEW_LOAD_SIZE, NEW_LOAD_SIZE)

    mode = src.stat().st_mode & 0o777
    dst.write_bytes(data)
    os.chmod(dst, mode)
    print(f"CPU patch: {src} -> {dst}, SHA256 {hashlib.sha256(data).hexdigest()}")
    print(f"UI {ui_ms} ms; camera scan 20 ms; UDP 12300 poll 20 ms; resume after low resin -> level")


if __name__ == "__main__":
    main()
