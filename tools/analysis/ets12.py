import json,statistics as st,math
S='/tmp/claude-1000/-home-dr-Claude-saturn4-fw/052dc525-3352-418d-b6ca-766ae2fd0911/scratchpad/'
res={int(k):v for k,v in json.load(open(S+'res12.json')).items()}
F=[L for L in sorted(res) if 51<=L<=100]   # fast tilt, peel clearly above noise
# first-reading phase after lift start
for L in F:
    s=res[L]['seg']; res[L]['phi']=min(t for t,v in s if t>=0)
# equivalent-time: template on 10 ms grid, -0.2..1.2 s
G=[i*0.01 for i in range(-20,121)]
def interp(seg,t):
    for (t1,v1),(t2,v2) in zip(seg,seg[1:]):
        if t1<=t<=t2: return v1+(v2-v1)*(t-t1)/(t2-t1)
A={L:res[L]['peak'] for L in F}
for it in range(8):
    tmpl=[]
    for g in G:
        pts=[]
        for L in F:
            for t,v in res[L]['seg']:
                if abs(t-g)<=0.015: pts.append(-v/A[L])
        tmpl.append(st.median(pts) if pts else 0)
    def f(t):
        if t<=G[0] or t>=G[-1]: return 0
        i=int((t-G[0])/0.01); w=(t-G[i])/0.01; return tmpl[i]*(1-w)+tmpl[i+1]*w
    for L in F:
        pts=[(f(t),-v) for t,v in res[L]['seg'] if -0.1<t<1.0]
        A[L]=sum(a*b for a,b in pts)/sum(a*a for a,b in pts)
    m=max(tmpl); A={L:A[L]*m for L in A}; tmpl=[x/m for x in tmpl]
pk=max(range(len(G)),key=lambda i:tmpl[i])
half=[G[i] for i in range(len(G)) if tmpl[i]>=0.5]
print('template peak at +%.2f s after lift start, width above 50%%: %.2f s (%.2f..%.2f)'%(G[pk],half[-1]-half[0],half[0],half[-1]))
print('template:',' '.join('%.2f:%.2f'%(G[i],tmpl[i]) for i in range(0,len(G),5) if -0.1<=G[i]<=1.0))
ratio=[res[L]['peak']/A[L] for L in F]
print('sampled peak / true peak: mean %.2f min %.2f max %.2f'%(st.mean(ratio),min(ratio),max(ratio)))
# scatter vs area model (linear fit) sampled vs reconstructed
def fit(y):
    x=[res[L]['area'] for L in F]; mx,my=st.mean(x),st.mean(y)
    b=sum((a-mx)*(c-my) for a,c in zip(x,y))/sum((a-mx)**2 for a in x); a0=my-b*mx
    r=[c-(a0+b*a) for a,c in zip(x,y)]; ss=sum((c-my)**2 for c in y)
    return a0,b,math.sqrt(st.mean([e*e for e in r])),1-sum(e*e for e in r)/ss
for name,y in (('sampled',[res[L]['peak'] for L in F]),('reconstructed',[A[L] for L in F])):
    a0,b,rms,r2=fit(y); print('%-13s F0 %.0f g (%.1f N)  kA %.3f g/mm2 (%.4f N/mm2)  rms %.0f g  R2 %.3f'%(name,a0,a0*0.00981,b,b*0.00981,rms,r2))
# phase dependence
ph=[(res[L]['phi'],res[L]['peak']/A[L]) for L in F]
for lo in (0,0.05,0.10,0.15):
    v=[r for p,r in ph if lo<=p<lo+0.05]; print('first reading +%.2f..%.2f s: n %d sampled/true %.2f'%(lo,lo+0.05,len(v),st.mean(v) if v else 0))
json.dump({'A':A,'tmpl':tmpl,'G':G},open(S+'ets12.json','w'))
