#!/usr/bin/env python3
"""Decode the 'D' rows (driver status) of a tilt trace (sglog/tilt_*.csv, TILTTRACE + TILTDRV).
D,<XACTUAL>,<DRV_STATUS>,<PWM_SCALE>; M = move (a = target, b = speed), P = position poll (4 ms).
Usage: tilt_drv.py tilt.csv [-v]   (-v: every D row)"""
import sys
rows = [l.rstrip().split(',') for l in open(sys.argv[1]) if l[0].isdigit()]
verbose = '-v' in sys.argv
P = [(float(r[0]), int(r[2])) for r in rows if r[1] == 'P']
def speed(t):  # steps/s from the P rows around t (+-40 ms)
    w = [(a, b) for a, b in P if abs(a - t) <= 40]
    return (w[-1][1] - w[0][1]) / ((w[-1][0] - w[0][0]) / 1000) if len(w) > 1 and w[-1][0] > w[0][0] else 0.0
t0 = None; n = 0
for r in rows:
    t = float(r[0])
    if r[1] == 'M':
        print(f"--- move at {t:.0f} ms: target {r[3]} speed {r[4]}")
        t0 = t
    elif r[1] == 'D':
        ds = int(r[3]) & 0xffffffff; ps = int(r[4]); x = int(r[2])
        cs = (ds >> 16) & 31; stst = ds >> 31; sg = ds & 0x3ff
        flags = ''.join(f for f, b in (('OT', 25), ('OTPW', 26), ('S2GA', 27), ('S2GB', 28), ('OLA', 29), ('OLB', 30)) if ds >> b & 1)
        n += 1
        if verbose:
            print(f"{t - (t0 or t):8.0f} ms x={x:7d} v={speed(t):9.0f} st/s  CS={cs:2d} PWM_SCALE={ps:3d} stst={stst} {flags}")
print(n, 'D rows')
