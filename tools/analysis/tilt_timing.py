#!/usr/bin/env python3
"""Tilt (X, axis 1) move timing from the chitu app log (8.16).
Usage: tilt_timing.py LOG [LOG..]   (or - for stdin; give log2 before log1)
One row per tilt move: phase (rel = release, ret = return), stage (1 = flag 2 / wait 1,
2 = the move after a stage 1, - = single), target deg, deg/s, steps, start ms (stepper_move),
idle ms ("axis 1 is idle"), duration, gap to the next command, and the logged rotate params.
Driver: fq6630 = TMC5160-like ramp chip; stage 1 is a separate move to the
absolute stage angle with TZEROWAIT 0, the next move is sent only after position_reached."""
import re, sys

R = re.compile(r'^\[(\w+)\]\[\w+\]\[\d+\]\[(\d+)\]:(.*)')
STEP_DEG = 0.00703125


def lines(paths):
    for p in paths:
        f = sys.stdin if p == '-' else open(p, errors='replace')
        for ln in f:
            m = R.match(ln)
            if m: yield m.group(1), int(m.group(2)), m.group(3).strip()


def main(paths):
    phase, params = '?', ''
    pend = []            # queued axis_move index 1 (pos, speed, flag, t): a two-stage move
                         # logs both axis_move lines before the axis thread sends stage 1
    cur = None           # move in progress
    prev_stage1 = False
    rows = []
    for mod, t, msg in lines(paths):
        if msg.startswith('lifting = '):
            phase = {'1': 'rel', '2': 'ret'}.get(msg.split()[2], phase)
        if (m3 := re.match(r'(sync_mode \d+ lift_angle.*|level_mode \d+ drop_angle.*)', msg)):
            params = m3.group(1)     # logged after the move's axis_move line(s), before stepper_move
        if (m4 := re.match(r'axis_move: index 1 (\S+) (\S+) (-?\d+) (\d+) flag (\w+)', msg)):
            pend.append((float(m4.group(1)), float(m4.group(2)), int(m4.group(5), 16), t))
        if (m5 := re.match(r'stepper_move:index 1 step (-?\d+) speed (\d+) wait (\d)', msg)):
            pos, spd, flag, ta = pend.pop(0) if pend else (None, None, 0, t)
            stage = '1' if flag & 2 else ('2' if prev_stage1 else '-')
            prev_stage1 = bool(flag & 2)
            if rows and rows[-1]['idle'] is not None and rows[-1]['gap'] is None:
                rows[-1]['gap'] = t - rows[-1]['idle']
            cur = dict(phase=phase, stage=stage, pos=pos, spd=spd, steps=int(m5.group(1)),
                       sps=int(m5.group(2)), wait=int(m5.group(3)), t0=t, ta=ta, idle=None,
                       gap=None, params=params)
            rows.append(cur)
            if not pend: params = ''
        if msg == 'axis 1 is idle' and cur and cur['idle'] is None:
            cur['idle'] = t
    print('phase stage  target  deg/s   steps  wait   start_ms      dur_ms  gap_ms  const_ms  params')
    for r in rows:
        dur = r['idle'] - r['t0'] if r['idle'] else None
        const = abs(r['steps']) / r['sps'] * 1000 if r['sps'] else 0
        print(f"{r['phase']:>4} {r['stage']:>5} {r['pos'] if r['pos'] is not None else '?':>7} "
              f"{r['spd'] if r['spd'] is not None else '?':>6} {r['steps']:>7} {r['wait']:>5} "
              f"{r['t0']:>10} {dur if dur is not None else '-':>10} "
              f"{r['gap'] if r['gap'] is not None else '-':>7} {const:9.0f}  {r['params']}")


if __name__ == '__main__':
    main(sys.argv[1:] or ['-'])
