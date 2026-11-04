#!/usr/bin/env python3
"""Build a UBIFS volume image with the same parameters as the original one.

Usage:
  tools/build_volume.py work customer build/customer.ubifs [--root DIR [--state FILE]]

Parameters (min I/O, LEB size, max LEB count, compression, fanout, log/orphan LEBs,
key hash, space fixup) are read from the original superblock in work/raw/<name>.bin.
The source tree defaults to work/fs/<name>; ownership/modes come from the fakeroot
state file work/fs/<name>.fakeroot written by unpack.py (or --state, for a modified
copy of the tree made under fakeroot, see tools/build_mod.sh).
After building, the new image's superblock is checked against the original
(in particular fmt_version: kernel 4.9 cannot mount UBIFS format 5).
"""

import argparse
import os
import struct
import subprocess
import sys

UBIFS_MAGIC = 0x06101831
SB_NODE = 6
COMPR = {0: "none", 1: "lzo", 2: "zlib", 3: "zstd"}
KEY_HASH = {0: "r5", 1: "test"}
SPACE_FIXUP = 0x04


def read_superblock(path):
    with open(path, "rb") as f:
        d = f.read(4096)
    magic, _crc, _sqnum, _len, node_type = struct.unpack_from("<IIQIB", d, 0)
    if magic != UBIFS_MAGIC or node_type != SB_NODE:
        sys.exit(f"{path}: no UBIFS superblock")
    b = 24
    (key_hash, key_fmt, flags, min_io, leb_size, leb_cnt, max_leb_cnt, max_bud_bytes,
     log_lebs, lpt_lebs, orph_lebs, jhead_cnt, fanout, lsave_cnt, fmt_version,
     default_compr) = struct.unpack_from("<2xBBIIIIIQIIIIIIIH", d, b)
    return dict(key_hash=key_hash, key_fmt=key_fmt, flags=flags, min_io=min_io,
                leb_size=leb_size, leb_cnt=leb_cnt, max_leb_cnt=max_leb_cnt,
                max_bud_bytes=max_bud_bytes, log_lebs=log_lebs, lpt_lebs=lpt_lebs,
                orph_lebs=orph_lebs, jhead_cnt=jhead_cnt, fanout=fanout,
                lsave_cnt=lsave_cnt, fmt_version=fmt_version, default_compr=default_compr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("work_dir")
    ap.add_argument("name")
    ap.add_argument("output")
    ap.add_argument("--root", help="source tree (default: <work>/fs/<name>)")
    ap.add_argument("--state", help="fakeroot state of the tree (default: <work>/fs/<name>.fakeroot)")
    args = ap.parse_args()

    orig = os.path.join(args.work_dir, "raw", args.name + ".bin")
    root = args.root or os.path.join(args.work_dir, "fs", args.name)
    state = args.state or os.path.join(args.work_dir, "fs", args.name + ".fakeroot")
    if args.root and not args.state:
        sys.exit("--root needs --state (the stock state file only matches the stock tree)")
    sb = read_superblock(orig)

    cmd = ["mkfs.ubifs",
           "-m", str(sb["min_io"]), "-e", str(sb["leb_size"]), "-c", str(sb["max_leb_cnt"]),
           "-x", COMPR[sb["default_compr"]], "-f", str(sb["fanout"]),
           "-l", str(sb["log_lebs"]), "-p", str(sb["orph_lebs"]),
           "-k", KEY_HASH[sb["key_hash"]],
           "-j", str(sb["max_bud_bytes"]),
           "-r", root, "-o", args.output]
    if sb["flags"] & SPACE_FIXUP:
        cmd.insert(1, "-F")
    if os.path.exists(state):
        cmd = ["fakeroot", "-i", state] + cmd
    else:
        print(f"warning: {state} missing, squashing all owners to root (-U)", file=sys.stderr)
        cmd.insert(1, "-U")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    if not os.path.exists("/usr/sbin/mkfs.ubifs") and subprocess.run(["which", "mkfs.ubifs"], capture_output=True).returncode:
        sys.exit("mkfs.ubifs not found")
    env = dict(os.environ, PATH=os.environ.get("PATH", "") + ":/usr/sbin:/sbin")
    print(" ".join(cmd))
    subprocess.run(cmd, check=True, env=env)

    new = read_superblock(args.output)
    print(f"\n{'field':<14} {'original':>12} {'new':>12}")
    bad = False
    for k in sb:
        mark = ""
        if sb[k] != new[k]:
            mark = "  <-- differs"
            # leb_cnt and lpt_lebs depend on the content; everything else must match
            if k not in ("leb_cnt", "lpt_lebs", "lsave_cnt"):
                bad = True
        print(f"{k:<14} {sb[k]:>12} {new[k]:>12}{mark}")
    size = os.path.getsize(args.output)
    print(f"\n{args.output}: {size} bytes = {size // sb['leb_size']} LEBs (original {sb['leb_cnt']})")
    if bad:
        sys.exit("superblock parameters differ from the original - do NOT flash this image")


if __name__ == "__main__":
    main()
