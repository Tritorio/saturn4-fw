#!/usr/bin/env python3
"""Unpack a SigmaStar/Chitu USB upgrade image (ChituUpgrade.bin).

Layout:
  0x0000  U-Boot script (text, ends with '%'), padded with 0xFF to 0x4000
  ...     chunks referenced by "fatload usb 0 <addr> $(UpgradeImage) <size> <offset>",
          each = payload + 8 ASCII hex chars (crc32 of payload), 0xFF gaps between them
  end     footer (0xFF padding + "12345678" + first 16 bytes of the script)

Output (in <out_dir>):
  script.txt        the upgrade script, exactly as in the image
  raw/<name>.bin    chunk payloads (CRC trailer stripped)
  footer.bin        everything after the last chunk
  manifest.json     offsets/sizes/crcs, used by pack.py
  fs/<name>/        extracted UBIFS volumes (with --extract-fs)

Usage:
  tools/unpack.py Saturn4Ultra_OTA/ChituUpgrade.bin work [--extract-fs]
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zlib

HEADER_SIZE = 0x4000
CRC_LEN = 8
UBIFS_MAGIC = b"\x31\x18\x10\x06"

FATLOAD_RE = re.compile(
    r"fatload usb 0 (0x[0-9a-fA-F]+) \$\(UpgradeImage\) (0x[0-9a-fA-F]+) (0x[0-9a-fA-F]+)")
PARTITION_RE = re.compile(r"^# File Partition: (\S+)")


def parse_script(script):
    """Return the chunks in script order: section name, load address, size, offset, script line."""
    chunks = []
    section = None
    for lineno, line in enumerate(script.splitlines()):
        m = PARTITION_RE.match(line)
        if m:
            section = m.group(1).removesuffix(".es")
            continue
        m = FATLOAD_RE.search(line)
        if m:
            chunks.append({
                "section": section,
                "load_addr": m.group(1),
                "size": int(m.group(2), 16),
                "offset": int(m.group(3), 16),
                "script_line": lineno,
            })
    # unique names: cis has two chunks -> cis_0, cis_1
    counts = {}
    for c in chunks:
        counts[c["section"]] = counts.get(c["section"], 0) + 1
    seen = {}
    for c in chunks:
        s = c["section"]
        if counts[s] > 1:
            c["name"] = f"{s}_{seen.get(s, 0)}"
            seen[s] = seen.get(s, 0) + 1
        else:
            c["name"] = s
    return chunks


def detect_type(payload):
    if payload[:4] == UBIFS_MAGIC:
        return "ubifs"
    if payload[:4] == b"\x27\x05\x19\x56":
        return "uimage"
    if payload[:8] == b"MSTARSEM":
        return "cis"
    if payload[4:8] in (b"IPL_", b"IPLN"):
        return "ipl"
    if payload[:5] == b"SSTAR":
        return "logo"
    return "raw"


def extract_ubifs(image, dest, state_file):
    """Extract a UBIFS image under fakeroot, keeping ownership/modes in a fakeroot state file."""
    ubireader = shutil.which("ubireader_extract_files") or os.path.join(
        os.path.dirname(sys.executable), "ubireader_extract_files")
    if not os.path.exists(ubireader):
        sys.exit("ubireader_extract_files not found: run 'source .venv/bin/activate' first")
    if os.path.exists(dest):
        shutil.rmtree(dest)
    # ubireader creates <dest> itself; -k needs (fake)root to apply uid/gid
    cmd = ["fakeroot", "-s", state_file, ubireader, "-k", "-o", dest, image]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr, file=sys.stderr)
        sys.exit(f"extraction failed for {image}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("out_dir")
    ap.add_argument("--extract-fs", action="store_true", help="also extract UBIFS volumes to <out_dir>/fs")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    header = data[:HEADER_SIZE]
    end = header.find(b"%")
    if end < 0:
        sys.exit("no end-of-script '%' marker in the first 16 KB")
    # keep the whole '%' line, as in the original
    nl = header.find(b"\n", end)
    script_bytes = header[: nl + 1 if nl >= 0 else end + 1]
    if header[len(script_bytes):].strip(b"\xff"):
        sys.exit("unexpected non-0xFF data after the script in the header")
    script = script_bytes.decode("ascii")

    chunks = parse_script(script)
    if not chunks:
        sys.exit("no fatload lines found in script")

    os.makedirs(os.path.join(args.out_dir, "raw"), exist_ok=True)
    with open(os.path.join(args.out_dir, "script.txt"), "wb") as f:
        f.write(script_bytes)

    ok = True
    print(f"{'name':<12} {'type':<7} {'offset':>10} {'size':>10}  crc")
    for c in chunks:
        blob = data[c["offset"]: c["offset"] + c["size"]]
        payload, trailer = blob[:-CRC_LEN], blob[-CRC_LEN:]
        crc = "%08x" % zlib.crc32(payload)
        c["crc32"] = crc
        c["crc_ok"] = trailer.decode("ascii", "replace") == crc
        c["type"] = detect_type(payload)
        c["payload_size"] = len(payload)
        c["sha256"] = hashlib.sha256(payload).hexdigest()
        c["file"] = f"raw/{c['name']}.bin"
        ok &= c["crc_ok"]
        with open(os.path.join(args.out_dir, c["file"]), "wb") as f:
            f.write(payload)
        print(f"{c['name']:<12} {c['type']:<7} {c['offset']:>#10x} {c['size']:>#10x}  "
              f"{crc} {'OK' if c['crc_ok'] else 'MISMATCH (trailer ' + repr(trailer) + ')'}")

    # gaps between chunks must be pure 0xFF padding, so pack.py can regenerate them
    by_offset = sorted(chunks, key=lambda c: c["offset"])
    prev = HEADER_SIZE
    for c in by_offset:
        if c["offset"] < prev:
            sys.exit(f"chunk {c['name']} overlaps the previous one")
        if data[prev:c["offset"]].strip(b"\xff"):
            sys.exit(f"non-0xFF data in gap before {c['name']} ({prev:#x}..{c['offset']:#x})")
        prev = c["offset"] + c["size"]
    footer = data[prev:]
    with open(os.path.join(args.out_dir, "footer.bin"), "wb") as f:
        f.write(footer)

    manifest = {
        "source": os.path.basename(args.image),
        "source_size": len(data),
        "source_md5": hashlib.md5(data).hexdigest(),
        "header_size": HEADER_SIZE,
        "align": 0x1000,
        "footer": "footer.bin",
        "chunks": chunks,
    }
    with open(os.path.join(args.out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    if args.extract_fs:
        fs_dir = os.path.join(args.out_dir, "fs")
        os.makedirs(fs_dir, exist_ok=True)
        for c in chunks:
            if c["type"] != "ubifs":
                continue
            print(f"extracting {c['name']} ...")
            extract_ubifs(os.path.join(args.out_dir, c["file"]),
                          os.path.join(fs_dir, c["name"]),
                          os.path.join(fs_dir, c["name"] + ".fakeroot"))

    print(f"\n{len(chunks)} chunks, footer {len(footer)} bytes -> {args.out_dir}")
    if not ok:
        sys.exit("CRC MISMATCH in at least one chunk")


if __name__ == "__main__":
    main()
