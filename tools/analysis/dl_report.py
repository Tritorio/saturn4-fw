#!/usr/bin/env python3
"""Dynamic lift report of one print: per layer the peel peak (job CSV g_min), the start speed and the switch angle (tilt trace S1 / DL2 / DL rows).
Usage: dl_report.py JOB.csv TILT_TRACE.csv [step]      (prints every `step`-th layer after the first 30; default 10)"""
import csv, collections, statistics as st, sys
T = 0.00703125
rows = list(csv.DictReader(open(sys.argv[1])))
step = int(sys.argv[3]) if len(sys.argv) > 3 else 10
cpu = [float(r['cpu']) for r in rows if r.get('cpu')]
if cpu: print('cpu column: n=%d mean %.1f max %.1f %%' % (len(cpu), st.mean(cpu), max(cpu)))
pk = collections.defaultdict(float)
for r in rows:
    if r['layer'] and r['g_min']:
        L = int(r['layer']); pk[L] = min(pk[L], float(r['g_min']))
tr = [l.rstrip().split(',') for l in open(sys.argv[2]) if l[0].isdigit()]
rel = []; cur = None
for r in tr:
    k = r[1]; x, a, b = int(r[2]), int(r[3]), int(r[4])
    if k == 'S1' and a > x: cur = {'start': b * T, 'sw': None, 'fast': None}; rel.append(cur)
    elif k == 'DL2' and cur: cur['fast'] = b * T
    elif k == 'DL' and cur and cur['sw'] is None: cur['sw'] = (x + 19911) * T
print('releases %d, layers with a peak %d' % (len(rel), len(pk)))
print('switched %d, never switched %d' % (sum(1 for r in rel if r['sw'] is not None), sum(1 for r in rel if r['sw'] is None)))
sa = [r['sw'] for r in rel if r['sw'] is not None]
if sa: print('switch angle deg: min %.1f median %.1f max %.1f' % (min(sa), st.median(sa), max(sa)))
vals = [-v for _, v in sorted(pk.items())]
print('peak g: min %.0f median %.0f max %.0f; > 1500 g in %d layers; > 2000 g in %d' % (min(vals), st.median(vals), max(vals), sum(v > 1500 for v in vals), sum(v > 2000 for v in vals)))
print('layer | peak g | start deg/s | switch deg')
for i, r in enumerate(rel):
    if i < 30 or i % step == 0:
        print('%3d  %5.0f  %5.1f  %s' % (i, -pk.get(i, 0), r['start'], '-' if r['sw'] is None else '%.1f' % r['sw']))
