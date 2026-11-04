#!/usr/bin/env python3
"""Timeline of a board-link trace written by libsgtap sgspi.c (sgcap/fpga_<boot>_<s>.csv, step 8.25).
Usage: fpga_trace.py FILE.csv [--all]     collapses runs of 0x3c image packets (513 or 2049 bytes) into one line.
Columns of the csv: ms (CLOCK_MONOTONIC, the app log's [tick] clock), tid, kind W/R/I, len, info (hex of the first bytes)."""
import sys
def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    rows = []
    for l in open(sys.argv[1]):
        if l.startswith('#') or l.startswith('ms,'):
            continue
        p = l.rstrip('\n').split(',', 4)
        if len(p) == 5:
            rows.append((int(p[0]), p[2], int(p[3]), p[4]))
    if not rows:
        print('no records'); return 1
    t0, i = rows[0][0], 0
    img = lambda r: r[1] == 'W' and r[3].startswith('3c') and r[2] in (513, 2049)
    while i < len(rows):
        ms, k, n, info = rows[i]
        if img(rows[i]):
            j = i
            while j + 1 < len(rows) and img(rows[j + 1]) and rows[j + 1][2] == n:
                j += 1
            cnt = j - i + 1
            dur = rows[j][0] - ms
            rate = cnt * n / dur / 1000 if dur else 0
            print(f'{(ms - t0) / 1000:9.3f}s IMAGE {cnt} x {n} B = {cnt * n} B in {dur} ms ({rate:.2f} MB/s) first={info[:24]}')
            i = j + 1
            continue
        if k == 'I':
            print(f'{(ms - t0) / 1000:9.3f}s IMAGE burst {n} B {info}')
        else:
            same = 1
            while i + same < len(rows) and rows[i + same][1:] == rows[i][1:] and rows[i + same][0] - ms < 20:
                same += 1
            print(f'{(ms - t0) / 1000:9.3f}s {k} len={n} {info[:40]}' + (f'  x{same}' if same > 1 else ''))
            i += same
            continue
        i += 1
if __name__ == '__main__':
    sys.exit(main())
