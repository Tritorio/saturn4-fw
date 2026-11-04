#!/usr/bin/env python3
"""Decode libsgtap capture files (cap_*.bin, see mods/src/sgtap/sgtap.c).

Usage:
  tools/sgcap_decode.py CAP.bin            summary per fd/direction + first records
  tools/sgcap_decode.py CAP.bin --dump N   hexdump the first N data records
  tools/sgcap_decode.py CAP.bin --fd F --dir rx|tx --raw OUT   concatenated payload
  --session K   only the K-th capture session in the file (a START record begins one;
                files can hold several, e.g. one per boot). Default: the last one.
"""
import argparse
import struct
import sys
from collections import defaultdict

MAGIC = 0x31544753
HDR = struct.Struct("<IQIBbH")
TYPES = {0: "READ", 1: "WRITE", 2: "OPEN", 3: "CLOSE", 4: "TCSETATTR", 5: "START", 6: "DROPPED"}
# termios c_cflag CBAUD codes -> baud
BAUD = {0o000015: 9600, 0o000016: 19200, 0o000017: 38400, 0o010001: 57600, 0o010002: 115200,
        0o010003: 230400, 0o010004: 460800, 0o010005: 500000, 0o010006: 576000,
        0o010007: 921600, 0o010010: 1000000, 0o010011: 1152000, 0o010012: 1500000,
        0o010013: 2000000, 0o010014: 2500000, 0o010015: 3000000}


def records(data):
    off = 0
    while off + HDR.size <= len(data):
        magic, ts, tid, typ, fd, ln = HDR.unpack_from(data, off)
        if magic != MAGIC:
            # resync on the next magic (should not happen)
            nxt = data.find(struct.pack("<I", MAGIC), off + 1)
            print(f"!! bad magic at {off:#x}, resync at {nxt:#x}", file=sys.stderr)
            if nxt < 0:
                return
            off = nxt
            continue
        yield ts, tid, typ, fd, data[off + HDR.size: off + HDR.size + ln]
        off += HDR.size + ln


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cap")
    ap.add_argument("--dump", type=int, default=0)
    ap.add_argument("--fd", type=int)
    ap.add_argument("--dir", choices=["rx", "tx"])
    ap.add_argument("--raw")
    ap.add_argument("--session", type=int, default=-1)
    a = ap.parse_args()
    data = open(a.cap, "rb").read()
    recs = list(records(data))
    if not recs:
        sys.exit("no records")
    starts = [i for i, r in enumerate(recs) if r[2] == 5] or [0]
    bounds = list(zip(starts, starts[1:] + [len(recs)]))
    if len(bounds) > 1:
        print(f"{len(bounds)} sessions in file, showing #{a.session % len(bounds)}"
              f" (--session K)", file=sys.stderr)
    b0, b1 = bounds[a.session]
    recs = recs[b0:b1]
    t0 = recs[0][0]
    if a.raw:
        typ = 0 if a.dir == "rx" else 1
        with open(a.raw, "wb") as f:
            for ts, tid, t, fd, p in recs:
                if t == typ and fd == a.fd:
                    f.write(p)
        return
    stats = defaultdict(lambda: [0, 0, None, None])  # count, bytes, first, last
    paths = {}
    dumped = 0
    for ts, tid, t, fd, p in recs:
        rel = (ts - t0) / 1e6
        if t in (0, 1):
            k = (fd, "rx" if t == 0 else "tx")
            s = stats[k]
            s[0] += 1; s[1] += len(p)
            s[2] = rel if s[2] is None else s[2]; s[3] = rel
            if dumped < a.dump:
                print(f"{rel:10.6f} fd{fd:<3} {k[1]} tid{tid} {len(p):4}  {p[:48].hex(' ')}")
                dumped += 1
        elif t == 2:
            paths[fd] = p.decode(errors="replace")
            print(f"{rel:10.6f} OPEN fd{fd} {paths[fd]} tid{tid}")
        elif t == 3:
            print(f"{rel:10.6f} CLOSE fd{fd}")
        elif t == 4:
            c_cflag = struct.unpack_from("<I", p, 8)[0]
            sp = struct.unpack_from("<I", p, len(p) - 4)[0]
            print(f"{rel:10.6f} TCSETATTR fd{fd} cflag={c_cflag:#o} baud={BAUD.get(sp, sp)}")
        elif t == 5:
            print(f"{rel:10.6f} START {p.decode(errors='replace')}")
        elif t == 6:
            print(f"{rel:10.6f} DROPPED {struct.unpack('<I', p)[0]} bytes")
    print(f"\n{len(recs)} records, {(recs[-1][0] - t0) / 1e6:.1f} s")
    for (fd, d), (n, b, f, l) in sorted(stats.items()):
        dur = max(l - f, 1e-9)
        print(f"fd{fd:<3} {paths.get(fd, '?'):<12} {d}: {n:7} records {b:9} bytes "
              f"{b / dur:9.1f} B/s  ({f:.1f}..{l:.1f} s)")


if __name__ == "__main__":
    main()
