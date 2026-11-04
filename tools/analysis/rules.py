import csv, math, sys
S=sys.argv[1]
r=list(csv.DictReader(open(S+'/log3/p.csv')))
L={}
for x in r:
    if x['layer']!='': L.setdefault(int(x['layer']),[]).append(x)
def slope(t,g,a,b):           # least-squares slope over rows a..b
    n=b-a+1; tm=sum(t[a:b+1])/n; gm=sum(g[a:b+1])/n
    den=sum((t[i]-tm)**2 for i in range(a,b+1)); return sum((t[i]-tm)*(g[i]-gm) for i in range(a,b+1))/den
def rule_grad(t,g,i,thr,win):
    k=int(win/0.5)
    return i>=k and abs(slope(t,g,i-k,i))<thr
def rule_remain(t,g,i,thr):    # exponential tail: remaining = s2*tau, tau from two consecutive 2 s windows
    if i<8: return False
    s1=slope(t,g,i-8,i-4); s2=slope(t,g,i-4,i)
    if abs(s2)<0.3: return True                       # flat within noise
    if s1==0 or s2/s1<=0 or s2/s1>=1: return False    # not a decaying tail
    tau=2.0/math.log(s1/s2); return abs(s2)*tau<thr
rules={'grad<1 g/s,1s':lambda t,g,i:rule_grad(t,g,i,1.0,1.0),
       'grad<0.5 g/s,2s':lambda t,g,i:rule_grad(t,g,i,0.5,2.0),
       'remaining<5 g':lambda t,g,i:rule_remain(t,g,i,5.0)}
groups=[(0,1),(2,14),(15,29),(30,49),(50,79),(80,129),(130,266)]
print('layers   | slicer wait | rule: mean wait, never, mean change after trigger, worst')
for a,b in groups:
    line=f"{a:3d}-{b:<3d} |"
    sw=[];
    out=[]
    for name,f in rules.items():
        waits=[];nev=0;res=[]
        for n in range(a,b+1):
            rs=L.get(n); 
            if not rs: continue
            t=[float(x['t_s']) for x in rs]; g=[float(x['g']) for x in rs]
            ex=next((i for i,x in enumerate(rs) if x['phase']=='exposure'),None)
            if ex is None: ex=next((i for i in range(4,len(g)) if g[i]<g[i-1]-150),None)
            if ex is None: continue
            if name.startswith('grad<1'): sw.append(t[ex]-t[0])
            trig=next((i for i in range(ex) if t[i]-t[0]>=1.0 and f(t,g,i)),None)
            if trig is None: nev+=1; continue
            waits.append(t[trig]-t[0]); res.append(max(abs(g[j]-g[trig]) for j in range(trig,ex)))
        mw=sum(waits)/len(waits) if waits else 0
        out.append(f" {name}: {mw:5.1f}s n{nev} chg{(sum(res)/len(res) if res else 0):5.1f}/{(max(res) if res else 0):5.1f}")
    print(line+f" {sum(sw)/len(sw):5.1f}s |"+' |'.join(out))
