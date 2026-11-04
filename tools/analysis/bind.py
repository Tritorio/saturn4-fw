import csv, json, sys, statistics as st
S=sys.argv[1]
def load(p): return list(csv.DictReader(open(p)))
def per_layer(rows):
    out={}
    for x in rows:
        if x['layer']=='' : continue
        out.setdefault(int(x['layer']),[]).append(x)
    return out
def analyse(rows, GRAD=1.0, WIN=1.0, MINW=1.0):
    L=per_layer(rows); res={}
    for n,rs in sorted(L.items()):
        t=[float(x['t_s']) for x in rs]; g=[float(x['g']) for x in rs]
        gmin=[float(x['g_min']) for x in rs]; gmax=[float(x['g_max']) for x in rs]
        ph=[x['phase'] for x in rs]
        # exposure start: first 'exposure' row, else first sharp fall after the rest
        ex=next((i for i,p in enumerate(ph) if p=='exposure'),None)
        if ex is None:
            ex=next((i for i in range(4,len(g)) if g[i]<g[i-1]-150),None)
        if ex is None or ex<3: continue
        lvl=st.median(g[max(0,ex-3):ex])           # settled level just before exposure
        peel=lvl-min(gmin[ex:])                    # pulling down (g)
        drop=max(gmax[ex:])-lvl                    # pushing up on the retract (g)
        # dynamic wait: from rest start (= app wait start) until slope over WIN < GRAD, >= MINW
        k=int(round(WIN/0.5)); dyn=None
        for i in range(k,ex):
            if t[i]-t[0] < MINW: continue
            if abs(g[i]-g[i-k])/(t[i]-t[i-k]) < GRAD:
                dyn=t[i]-t[0]; trig=i; break
        resid=None if dyn is None else max(abs(g[j]-g[trig]) for j in range(trig,ex))
        res[n]=dict(rest=t[ex]-t[0], lvl=lvl, peel=peel, drop=drop, dyn=dyn, resid=resid)
    return res
A=analyse(load(S+'/log3/p.csv'))
B=analyse(load(S+'/log2/20261019_164012_Force_sensor_test.csv'))
U={int(x['layer_index']):x for x in load('UVTools/Piramidi 3x_peel_force.csv')}
json.dump({'A':A,'B':B},open(S+'/bind.json','w'))
print("idx  area   perim  wait  rest | lvl    peel   drop | blank peel drop | dyn  resid")
for n in sorted(A):
    if n<16 or n%10==0:
        a=A[n]; u=U[n]; b=B.get(n)
        bs=f"{b['peel']:6.0f} {b['drop']:5.0f}" if b else "     -     -"
        d='never' if a['dyn'] is None else f"{a['dyn']:.1f}"
        r='' if a['resid'] is None else f"{a['resid']:.1f}"
        print(f"{n:3d} {float(u['area_mm2']):6.0f} {float(u['perimeter_mm']):5.0f} {u['wait_before_cure_s']:>5} {a['rest']:5.1f} | {a['lvl']:6.1f} {a['peel']:6.0f} {a['drop']:6.0f} | {bs} | {d:>5} {r}")
print(len(A),'layers analysed of',len(U))
