import csv, sys, warnings  # needs numpy + scipy (system python3, not the .venv)
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

for path in sys.argv[1:]:
    print("\n==", path.split('/')[-1])
    print("layer  actual  how      A(.3/2) A(.5/1)  B(.5g) B(.3g) B(1g)   tau   residual_at_end(g)")
    for w in waits(path):
        t, g = w['t'], w['g']
        if t[-1] < 3: continue
        mn = 2.0
        a1 = slope_rule(t, g, 0.3, 2.0, mn); a2 = slope_rule(t, g, 0.5, 1.0, mn)
        b5 = remain_rule(t, g, 0.5, mn); b3 = remain_rule(t, g, 0.3, mn); b10 = remain_rule(t, g, 1.0, mn)
        p = fit(t[t >= 1.0], g[t >= 1.0])
        tau = f"{p[2]:5.1f}" if p is not None else "  -  "
        res = f"{g[-2:].mean() - p[0]:+6.2f}" if p is not None else "   -  "
        print(f"{w['layer']:4d} {w['wait']:7.1f}  {w['how'][:7]:7s} {f(a1)}   {f(a2)}   {f(b5)}  {f(b3)}  {f(b10)}  {tau}  {res}")
