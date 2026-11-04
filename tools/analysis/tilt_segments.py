#!/usr/bin/env python3
"""Per motion segment statistics of a tilt trace with D rows (tilt_drv.py decodes single rows).
A segment = consecutive P rows where XACTUAL keeps changing (gap < 150 ms). Prints time, direction, median speed (steps/s),
duration, PWM_SCALE first/mean/min/max, CS_ACTUAL first/last, open-load row share.   Usage: tilt_segments.py tilt.csv [-v]"""
import sys, statistics as st
rows = [l.rstrip().split(',') for l in open(sys.argv[1]) if l[0].isdigit()]
P = [(float(r[0]), int(r[2])) for r in rows if r[1] == 'P']
D = []
for r in rows:
    if r[1] == 'D':
        ds = int(r[3]) & 0xffffffff
        D.append((float(r[0]), int(r[2]), int(r[4]), (ds >> 16) & 31, ds >> 31, (ds >> 29) & 3))
segs = []; cur = [P[0]]
for p in P[1:]:
    if p[1] != cur[-1][1]:
        cur.append(p)
    elif p[0] - cur[-1][0] > 150:
        if len(cur) > 5: segs.append(cur)
        cur = [p]
    elif p[0] - cur[-1][0] > 0 and p[1] == cur[-1][1]:
        cur.append(p) if False else None
if len(cur) > 5: segs.append(cur)
print(" t_s    dir  speed(st/s)  dur_s  PWMs first/mean/min/max   CS first-last  OL%")
for s in segs:
    t0, t1 = s[0][0], s[-1][0]
    v = [(b[1]-a[1]) / ((b[0]-a[0]) / 1000) for a, b in zip(s, s[1:]) if b[0] - a[0] > 20]
    if not v: continue
    sp = st.median(v)
    d = [x for x in D if t0 <= x[0] <= t1 and not x[4]]
    if not d: continue
    ps = [x[2] for x in d]
    print(f"{t0/1000:8.1f} {'+' if sp>0 else '-'} {abs(sp):8.0f} {(t1-t0)/1000:6.2f}   {ps[0]:3d} {sum(ps)/len(ps):5.1f} {min(ps):3d} {max(ps):3d}   {d[0][3]:2d}-{d[-1][3]:2d}   {100*sum(1 for x in d if x[5])/len(d):3.0f}")
