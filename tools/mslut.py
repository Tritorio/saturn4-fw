#!/usr/bin/env python3
"""TMC5130 microstep wave table (MSLUT0-7 0x60-0x67, MSLUTSEL 0x68, MSLUTSTART 0x69) decode / encode.
Quarter wave: ofs(0) = START_SIN, ofs(i+1) = ofs(i) + base(W of the segment) + bit i  (base W: 0 -> -1, 1 -> 0, 2 -> +1, 3 -> +2),
segments: i < X1: W0, < X2: W1, < X3: W2, else W3.
  mslut.py decode            decode the chip default and compare with 247 * sin
  mslut.py wave A_PERCENT    table for f = sin(t) + a sin(3t), peak kept = 247; prints C words
"""
import math, sys
DEF_LUT = [0xAAAAB554, 0x4A9554AA, 0x24492929, 0x10104222, 0xFBFFFFFF, 0xB5BB777D, 0x49295556, 0x00404222]
DEF_SEL = 0xFFFF8056
DEF_START = 0x00F70000
BASE = {0: -1, 1: 0, 2: 1, 3: 2}

def decode(lut, sel, start):
    x1, x2, x3 = (sel >> 8) & 255, (sel >> 16) & 255, (sel >> 24) & 255
    w = [(sel >> (2 * k)) & 3 for k in range(4)]
    ofs = [start & 255]
    for i in range(256):
        seg = 0 if i < x1 else 1 if i < x2 else 2 if i < x3 else 3
        b = (lut[i // 32] >> (i % 32)) & 1
        ofs.append(ofs[-1] + BASE[w[seg]] + b)
    return ofs  # 257 entries

def encode(target, x=(None, None, None)):
    """target: 257 wanted values (quarter wave 0..256). Returns (lut, sel, start, maxdev). Segment boundaries: the 3 boundaries and the 4 W
    are searched so that every wanted increment fits {base, base+1} within +-1 LSB error diffusion; minimal max deviation wins."""
    best = None
    start = (247 << 16) | (round(target[0]) & 255)
    for x1 in range(16, 200, 16):
        for x2 in range(x1 + 16, 256, 16):
            for x3 in (x2, 255):
                for w in [(a, b, c, d) for a in range(4) for b in range(4) for c in range(4) for d in range(4)]:
                    ofs = [round(target[0])]; bits = []; dev = 0; ok = True
                    for i in range(256):
                        seg = 0 if i < x1 else 1 if i < x2 else 2 if i < x3 else 3
                        base = BASE[w[seg]]
                        want = target[i + 1] - ofs[-1]
                        b = 1 if want - base > 0.5 else 0
                        inc = base + b
                        if abs(want - inc) > 1.6:
                            ok = False; break
                        ofs.append(ofs[-1] + inc); bits.append(b)
                        dev = max(dev, abs(ofs[-1] - target[i + 1]))
                    if ok and (best is None or dev < best[0]):
                        lut = [0] * 8
                        for i, b in enumerate(bits):
                            lut[i // 32] |= b << (i % 32)
                        sel = (x3 << 24) | (x2 << 16) | (x1 << 8) | w[3] << 6 | w[2] << 4 | w[1] << 2 | w[0]
                        best = (dev, lut, sel, start)
                        if dev <= 0.5:
                            return lut, sel, start, dev
    return best[1], best[2], best[3], best[0]

def wave(a):
    f = lambda t: math.sin(t) + a * math.sin(3 * t)
    pk = max(f(math.pi / 2 * i / 256) for i in range(257))
    return [247 * f(math.pi / 2 * i / 256) / pk for i in range(257)]

def table_at(ofs, m):
    """signed table value for MSCNT m (0..1023): quadrant mirror of the quarter wave ofs(0..256)"""
    m %= 1024
    q, r = divmod(m, 256)
    v = ofs[r] if q in (0, 2) else ofs[256 - r]
    return v if q < 2 else -v

def expected(pct, mscnt):
    """(A, B) expected in MSCURACT for wave pct at MSCNT"""
    if pct == 0:
        o = decode(DEF_LUT, DEF_SEL, DEF_START)
    else:
        o = decode(*encode(wave(pct / 100))[:3])
    return table_at(o, mscnt), table_at(o, mscnt + 256)

if __name__ == '__main__':
    if sys.argv[1] == 'check':     # check PCT MSCNT A B: compare a `tiltreg cur` result with the table
        pct, m, a, b = (int(x) for x in sys.argv[2:6])
        ea, eb = expected(pct, m)
        print('wave %d%% MSCNT %d: expected A %d B %d, read A %d B %d -> %s' % (pct, m, ea, eb, a, b, 'OK' if abs(ea - a) <= 2 and abs(eb - b) <= 2 else 'MISMATCH (run tiltreg stock)'))
    elif sys.argv[1] == 'decode':
        o = decode(DEF_LUT, DEF_SEL, DEF_START)
        err = [o[i] - 247 * math.sin(math.pi / 2 * i / 256) for i in range(257)]
        print('ofs(0)', o[0], 'ofs(128)', o[128], 'ofs(255)', o[255], 'ofs(256)', o[256], ' max |ofs - 247 sin| = %.2f' % max(abs(e) for e in err))
    else:
        a = float(sys.argv[2]) / 100
        t = wave(a)
        lut, sel, start, dev = encode(t)
        o = decode(lut, sel, start)
        print('a=%.2f maxdev %.2f ofs256 %d' % (a, max(abs(o[i] - t[i]) for i in range(257)), o[256]))
        print('LUT', ', '.join('0x%08x' % v for v in lut), 'SEL 0x%08x' % sel, 'START 0x%08x' % start)
