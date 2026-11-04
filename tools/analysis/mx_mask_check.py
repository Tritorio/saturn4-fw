#!/usr/bin/env python3
"""Check the Multi Exposure masker output (sgmxtest, mods/src/sgtap/sgmxmask.h) against the original dump:
  mx_mask_check.py ORIGINAL.bin PREFIX    PREFIX_<litmask>.bin for litmask 0..15 are decoded again; per mask a quadrant q (0 TL, 1 TR, 2 BL, 3 BR)
  must be identical to the original when its bit is set and fully black otherwise; every half row 7560 px, every run byte <= 72."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import numpy as np
import lcd_stream as L


def quads(c):
    h, w = c.shape
    y, x = h // 2, w // 2
    return [c[:y, :x], c[:y, x:], c[y:, :x], c[y:, x:]]


def main(orig, prefix):
    s = open(orig, 'rb').read()
    q0 = quads(L.decode(s))
    bad = 0
    for m in range(16):
        t = open(f'{prefix}_{m}.bin', 'rb').read()
        arr = np.frombuffer(t, np.uint8)
        runs_ok = int(arr[arr < 0x80].max()) <= 72
        q1 = quads(L.decode(t))
        for q in range(4):
            if m >> q & 1:
                ok = np.array_equal(q0[q], q1[q])
            else:
                ok = bool((q1[q] == 0x80).all())
            if not ok:
                print(f'mask {m} quadrant {q}: FAIL')
                bad += 1
        if not runs_ok or len(t) > len(s):
            print(f'mask {m}: runs ok {runs_ok}, length {len(t)} vs {len(s)}: FAIL')
            bad += 1
    print('masker check:', 'ALL OK' if not bad else f'{bad} FAILED', f'(16 masks, {os.path.basename(orig)})')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:3]) if len(sys.argv) > 2 else print(__doc__))
