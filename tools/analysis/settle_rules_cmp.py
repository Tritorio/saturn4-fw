import sys, numpy as np
sys.path.insert(0, __file__.rsplit('/',1)[0])
from settle_rules_lib import *
rules = {
 "A .5/1": lambda t,g: slope_rule(t,g,0.5,1.0,2.0),
 "A .3/2": lambda t,g: slope_rule(t,g,0.3,2.0,2.0),
 "A .3/3": lambda t,g: slope_rule(t,g,0.3,3.0,2.0),
 "A .2/3": lambda t,g: slope_rule(t,g,0.2,3.0,2.0),
 "B .5g":  lambda t,g: remain_rule(t,g,0.5,2.0),
}
for path in sys.argv[1:]:
    W = [w for w in waits(path) if w['t'][-1] >= 8 and w['wait'] < 150]
    print("\n==", path.split('/')[-1], f"({len(W)} waits of 8..150 s)")
    # excess = reading (mean of the 1 s around the rule's time) minus the plateau (mean of the last 1.5 s of the logged wait)
    for name, rl in rules.items():
        ts, ex, nb = [], [], 0
        for w in W:
            t, g = w['t'], w['g']; plat = g[t > t[-1] - 1.5].mean()
            x = rl(t, g)
            if x is None: nb += 1; continue
            m = (t > x - 0.6) & (t <= x + 1e-9)
            ts.append(x / w['wait']); ex.append(g[m].mean() - plat)
        if ts:
            print(f"{name:7s} fires in {len(ts):2d}/{len(W)}  time = {100*np.median(ts):5.1f}% of the actual wait (median)   excess over the plateau: median {np.median(ex):+5.2f} g, worst {max(ex):+6.2f} g   (not reached in the log: {nb})")
        else: print(f"{name:7s} never inside the logged wait")
