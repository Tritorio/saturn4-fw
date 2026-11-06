import csv, sys, warnings
import numpy as np
from scipy.optimize import curve_fit
warnings.filterwarnings("ignore")

def waits(path):
    rows = list(csv.reader(open(path)))
    h = rows[0]; ix = {n: i for i, n in enumerate(h)}
    d = rows[2:]
    out = []
    for k, r in enumerate(d):
        if r[ix['wait_end']] and r[ix['wait_s']]:
            L = r[ix['layer']]
            j = k - 1
            while j >= 0 and d[j][ix['layer']] == L and d[j][ix['phase']] == 'layer':
                j -= 1
            seg = d[j + 1:k]
            if len(seg) < 4: continue
            t = np.array([float(x[ix['t_s']]) for x in seg]); g = np.array([float(x[ix['g']]) for x in seg])
            out.append(dict(layer=int(L), wait=float(r[ix['wait_s']]), how=r[ix['wait_end']], t=t - t[0], g=g))
    return out

def slope_rule(t, g, grad, win, mn):
    """first time the |LSQ slope| of the last win s < grad (rows are 0.5 s means), time >= max(win, mn)"""
    for i in range(len(t)):
        if t[i] < max(win, mn): continue
        m = (t > t[i] - win - 1e-9) & (t <= t[i] + 1e-9)
        if m.sum() < 3: continue
        s = np.polyfit(t[m], g[m], 1)[0]
        if abs(s) < grad: return t[i]
    return None

def expf(t, f, a, tau): return f + a * np.exp(-t / tau)

def fit(t, g):
    try:
        p, _ = curve_fit(expf, t, g, p0=[g[-1], g[0] - g[-1], 3.0], bounds=([-1e5, -1e6, 0.2], [1e5, 1e6, 60]), maxfev=2000)
        return p
    except Exception:
        return None

def remain_rule(t, g, tol, mn, t_skip=1.0):
    for i in range(len(t)):
        if t[i] < max(mn, t_skip + 2.0): continue
        m = t >= t_skip
        tt, gg = t[m & (t <= t[i] + 1e-9)], g[m & (t <= t[i] + 1e-9)]
        if len(tt) < 6: continue
        p = fit(tt, gg)
        if p is None: continue
        rem = abs(expf(t[i], *p) - p[0])
        if rem < tol: return t[i]
    return None

def f(x): return "  -  " if x is None else f"{x:5.1f}"

