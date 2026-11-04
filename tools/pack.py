#!/usr/bin/env python3
"""Repack a SigmaStar/Chitu USB upgrade image from a directory made by unpack.py.

Usage:
  tools/pack.py work out/ChituUpgrade.bin
  tools/pack.py work out/ChituUpgrade.bin --replace customer=build/customer.ubifs
  tools/pack.py work out/ChituUpgrade.bin --only customer --replace customer=build/customer.ubifs
  tools/pack.py work out/ChituUpgrade.bin --version V1.5.9.1

--replace NAME=FILE  use FILE as payload for chunk NAME (sizes in the script are updated)
--only NAME[,NAME]   keep only these partition sections in the script (+ the header lines);
                     nothing else is erased or written on the printer
--version VER        set #upgrade_bin_version

Safety: chunks that hold boot code (cis, ipl, ipl_cust, uboot) can never be replaced.
"""

import argparse
import hashlib
import json
import os
import re
import sys
import zlib

PROTECTED = {"cis_0", "cis_1", "ipl", "ipl_cust", "uboot"}
FOOTER_MAGIC = b"12345678"
LEB_SIZE = 126976
SCRIPT_MAX = 4096

FATLOAD_RE = re.compile(
    r"(fatload usb 0 0x[0-9a-fA-F]+ \$\(UpgradeImage\) )(0x[0-9a-fA-F]+) (0x[0-9a-fA-F]+)")
PARTITION_RE = re.compile(r"^# File Partition: (\S+)")


def align(n, a):
    return (n + a - 1) // a * a


def hex_like(old_token, value):
    """Format value as hex using the same letter case as old_token."""
    s = "%x" % value
    return "0x" + (s.upper() if old_token[2:] != old_token[2:].lower() else s)


def split_sections(lines):
    """[(section_name or None for header, [lines])]"""
    sections = [(None, [])]
    for line in lines:
        m = PARTITION_RE.match(line)
        if m:
            sections.append((m.group(1).removesuffix(".es"), []))
        sections[-1][1].append(line)
    return sections


def replace_size_tokens(line, old_size, new_size):
    """Replace hex tokens equal to old_size (any case) by new_size, keeping case style."""
    def sub(m):
        tok = m.group(0)
        return hex_like(tok, new_size) if int(tok, 16) == old_size else tok
    return re.sub(r"0x[0-9a-fA-F]+", sub, line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("work_dir")
    ap.add_argument("output")
    ap.add_argument("--replace", action="append", default=[], metavar="NAME=FILE")
    ap.add_argument("--only", help="comma separated partition sections to keep (e.g. customer)")
    ap.add_argument("--version", help="new #upgrade_bin_version value")
    args = ap.parse_args()

    manifest = json.load(open(os.path.join(args.work_dir, "manifest.json")))
    chunks = manifest["chunks"]
    by_name = {c["name"]: c for c in chunks}
    header_size = manifest["header_size"]
    alignment = manifest["align"]

    # --- payloads ------------------------------------------------------------
    replacements = {}
    for r in args.replace:
        name, _, path = r.partition("=")
        if name not in by_name:
            sys.exit(f"unknown chunk '{name}' (known: {', '.join(by_name)})")
        if name in PROTECTED:
            sys.exit(f"refusing to replace boot-critical chunk '{name}'")
        replacements[name] = path

    payloads = {}
    for c in chunks:
        path = replacements.get(c["name"], os.path.join(args.work_dir, c["file"]))
        payloads[c["name"]] = open(path, "rb").read()
        if c["name"] not in replacements and hashlib.sha256(payloads[c["name"]]).hexdigest() != c["sha256"]:
            print(f"warning: {path} differs from the unpacked original", file=sys.stderr)

    # --- script --------------------------------------------------------------
    script = open(os.path.join(args.work_dir, "script.txt"), "rb").read().decode("ascii")
    lines = script.split("\n")
    trailing_newline = lines[-1] == ""
    if trailing_newline:
        lines = lines[:-1]
    sections = split_sections(lines)

    keep = None
    if args.only:
        keep = set(args.only.split(","))
        known = {s for s, _ in sections if s}
        if keep - known:
            sys.exit(f"unknown section(s) {keep - known} (known: {', '.join(sorted(known))})")
        if keep & {"cis", "set_partition", "ipl", "ipl_cust", "uboot", "set_config"}:
            sys.exit("--only must not include boot-critical or layout/env sections")
        # header (comments) + kept sections + the end-of-script line
        end_line = sections[-1][1][-1]
        new_sections = [s for s in sections if s[0] is None or s[0] in keep]
        if new_sections[-1][1][-1] != end_line:
            new_sections.append(("__end__", [end_line]))
        sections = new_sections

    kept_names = {s for s, _ in sections}
    used = [c for c in chunks if c["section"] in kept_names]

    # chunk layout: same order as the original, aligned, packed after the header
    offset = header_size
    layout = {}
    for c in sorted(used, key=lambda c: c["offset"]):
        size = len(payloads[c["name"]]) + 8
        layout[c["name"]] = (offset, size)
        offset = align(offset + size, alignment)
    data_end = offset  # aligned end of last chunk

    # rewrite sizes/offsets in each section
    out_lines = []
    for name, sec_lines in sections:
        sec_chunks = [c for c in used if c["section"] == name]
        new_lines = list(sec_lines)
        for c in sec_chunks:
            new_off, new_size = layout[c["name"]]
            old_payload = c["payload_size"]
            new_payload = new_size - 8
            # find the fatload line of this chunk inside this section
            idx = next(i for i, l in enumerate(new_lines)
                       if FATLOAD_RE.search(l) and int(FATLOAD_RE.search(l).group(3), 16) == c["offset"])
            m = FATLOAD_RE.search(new_lines[idx])
            new_lines[idx] = (new_lines[idx][:m.start()] + m.group(1)
                              + hex_like(m.group(2), new_size) + " " + hex_like(m.group(3), new_off)
                              + new_lines[idx][m.end():])
            if new_payload != old_payload:
                # following lines of this chunk (until next fatload): crccheck / write sizes
                j = idx + 1
                while j < len(new_lines) and not FATLOAD_RE.search(new_lines[j]):
                    new_lines[j] = replace_size_tokens(new_lines[j], c["size"], new_size)
                    new_lines[j] = replace_size_tokens(new_lines[j], old_payload, new_payload)
                    j += 1
                if c["name"] == "kernel":
                    sys.exit("kernel size changed: bootcmd (set_config) would need updating - not supported")
        out_lines.extend(new_lines)

    if args.version:
        out_lines = [re.sub(r"^#upgrade_bin_version=.*$", "#upgrade_bin_version=" + args.version, l)
                     for l in out_lines]

    new_script = ("\n".join(out_lines) + ("\n" if trailing_newline else "")).encode("ascii")
    if len(new_script) >= SCRIPT_MAX:
        sys.exit(f"script too large ({len(new_script)} bytes, must be < {SCRIPT_MAX})")
    if b"%" not in new_script:
        sys.exit("end-of-script '%' line missing")

    # --- sanity checks on UBI volumes ----------------------------------------
    creates = dict(re.findall(r"ubi create (\S+) (0x[0-9a-fA-F]+)", script))
    for c in used:
        if c["type"] == "ubifs":
            n = len(payloads[c["name"]])
            vol = int(creates[c["section"]], 16)
            if n % LEB_SIZE:
                sys.exit(f"{c['name']}: size {n:#x} is not a multiple of the LEB size {LEB_SIZE}")
            if n > vol:
                sys.exit(f"{c['name']}: image {n:#x} larger than UBI volume {vol:#x}")
            if payloads[c["name"]][:4] != b"\x31\x18\x10\x06":
                sys.exit(f"{c['name']}: not a UBIFS image")

    # --- assemble ------------------------------------------------------------
    out = bytearray(b"\xff" * data_end)
    out[:len(new_script)] = new_script
    for c in used:
        off, size = layout[c["name"]]
        p = payloads[c["name"]]
        out[off:off + size] = p + (b"%08x" % zlib.crc32(p))
    # footer: 0xFF up to alignment (already there) + magic + first 16 bytes of the script
    last_end = max(off + size for off, size in layout.values())
    out = out[:align(last_end, alignment)] + FOOTER_MAGIC + new_script[:16]

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(out)

    print(f"{'name':<12} {'offset':>10} {'size':>10}  crc")
    for c in sorted(used, key=lambda c: layout[c['name']][0]):
        off, size = layout[c["name"]]
        tag = " (replaced)" if c["name"] in replacements else ""
        print(f"{c['name']:<12} {off:>#10x} {size:>#10x}  {zlib.crc32(payloads[c['name']]):08x}{tag}")
    md5 = hashlib.md5(out).hexdigest()
    print(f"\n{args.output}: {len(out)} bytes, md5 {md5}")
    if md5 == manifest["source_md5"]:
        print("IDENTICAL to the original image")


if __name__ == "__main__":
    main()
