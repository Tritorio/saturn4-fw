import csv,json,sys,statistics as st
S='/tmp/claude-1000/-home-dr-Claude-saturn4-fw/052dc525-3352-418d-b6ca-766ae2fd0911/scratchpad/'
b=json.load(open(S+'bind.json'))['A']
rows=list(csv.DictReader(open('backups/sglog/sglog/20261019_181114_Piramidi_3x.csv')))
uv={int(r['layer_index']):r for r in csv.DictReader(open('UVTools/Piramidi 3x_peel_force.csv'))}
akey=[k for k in next(iter(uv.values())) if 'area' in k.lower()][0]
out={}
for L in range(15,130):
    k=str(L)
    if k not in b: continue
    lvl,peel=b[k]['lvl'],b[k]['peel']
    lift=[r for r in rows if r['layer']==k and r['phase']=='lift']
    if not lift: continue
    mins=[float(r['g_min']) for r in lift]
    i=min(range(len(mins)),key=lambda j:mins[j])
    # reading just before / after the minimum (row last value, next row min)
    before=float(lift[i-1]['g']) if i>0 else None
    after=float(lift[i+1]['g_min']) if i+1<len(lift) else None
    A=float(uv[L][akey]); model=1019+0.733*A
    half=sum(1 for m in mins if m<lvl-0.5*peel)
    out[L]=(peel,model,half,before,after,lvl)
for name,rng in (('slow 15-49',range(15,50)),('fast 50-129',range(50,130))):
    r=[(out[L][0]/out[L][1]) for L in rng if L in out]
    h=[out[L][2] for L in rng if L in out]
    nb=[ (out[L][5]-out[L][4])/out[L][0] for L in rng if L in out and out[L][4] is not None]
    print(name,'n',len(r),'peel/model mean %.2f sd %.2f (cv %.0f%%)'%(st.mean(r),st.stdev(r),100*st.stdev(r)/st.mean(r)),
      'rows<50%%peak mean %.1f'%st.mean(h),'next reading/peak %.2f'%st.mean(nb))
# adjacent-layer jitter: same area nearly, ratio of consecutive layers
for name,rng in (('slow',range(16,50)),('fast',range(51,130))):
    d=[abs(out[L][0]/out[L][1]-out[L-1][0]/out[L-1][1]) for L in rng if L in out and L-1 in out]
    print(name,'layer-to-layer change of peel/model: median %.2f max %.2f'%(st.median(d),max(d)))
# --- periodicity
import math
start={}
for r in rows:
    if r['layer'] and r['layer'] not in start: start[r['layer']]=float(r['t_s'])
tank={}
for r in rows:
    if r['layer'] and r['tank_c']: tank.setdefault(r['layer'],[]).append(float(r['tank_c']))
def per(L): return start.get(str(L+1),0)-start.get(str(L),0)
print('layer period:',[round(per(L),3) for L in (20,40,60,100,128)])
res={L:out[L][0]/out[L][1] for L in out}
def spec(seq,lo,hi):
    best=[]
    for P10 in range(25,400):
        P=P10/10; w=2*math.pi/P
        xs=list(seq); m=st.mean(v for _,v in xs)
        c=sum((v-m)*math.cos(w*L) for L,v in xs); s=sum((v-m)*math.sin(w*L) for L,v in xs)
        best.append((2*math.hypot(c,s)/len(xs),P))
    best.sort(reverse=True); return best[:4]
for name,rng in (('slow',range(15,50)),('fast',range(50,130))):
    sq=[(L,res[L]) for L in rng if L in res]
    lv=[(L,out[L][5]) for L in rng if L in out]
    print(name,'peel/model peaks (amp,period in layers):',[(round(a,3),p) for a,p in spec(sq,0,0)])
    print(name,'level peaks (g):',[(round(a,1),p) for a,p in spec(lv,0,0)])
    print(name,'tank_c range', min(min(tank[str(L)]) for L in rng if str(L) in tank), max(max(tank[str(L)]) for L in rng if str(L) in tank))
for L in range(50,90): print(L, '%.3f'%per(L), '%.2f'%res.get(L,0), '%.0f'%out[L][5] if L in out else '', '%.2f'%st.mean(tank[str(L)]))
print('---')
def corr(a,b):
    ma,mb=st.mean(a),st.mean(b); return sum((x-ma)*(y-mb) for x,y in zip(a,b))/math.sqrt(sum((x-ma)**2 for x in a)*sum((y-mb)**2 for y in b))
for name,rng in (('slow',range(15,50)),('fast',range(50,130)),('all',range(15,130))):
    Ls=[L for L in rng if L in res and str(L) in tank]
    print(name,'corr peel/model vs tank_c %.2f'%corr([res[L] for L in Ls],[st.mean(tank[str(L)]) for L in Ls]),
          ' level vs tank_c %.2f'%corr([out[L][5] for L in Ls],[st.mean(tank[str(L)]) for L in Ls]))
# detrended short-period search
Ls=[L for L in range(50,130) if L in res]
det=[]
for L in Ls:
    nb=[res[k] for k in range(L-4,L+5) if k in res]; det.append((L,res[L]-st.mean(nb)))
best=[]
for P10 in range(20,200):
    P=P10/10; w=2*math.pi/P
    c=sum(v*math.cos(w*L) for L,v in det); s=sum(v*math.sin(w*L) for L,v in det)
    best.append((round(2*math.hypot(c,s)/len(det),3),P))
best.sort(reverse=True); print('detrended fast, top periods (amp, layers):',best[:6])
# lag-1 autocorrelation of detrended: -ve => alternating
v=[x for _,x in det]; print('lag1 autocorr %.2f lag2 %.2f'%(corr(v[:-1],v[1:]),corr(v[:-2],v[2:])))
