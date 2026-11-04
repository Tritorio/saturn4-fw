import json,re,bisect,statistics as st
S='/tmp/claude-1000/-home-dr-Claude-saturn4-fw/052dc525-3352-418d-b6ca-766ae2fd0911/scratchpad/'
rd=json.load(open(S+'cap12.json')); T=[t/1e6 for t,v in rd]; V=[v/100.252 for t,v in rd]
P12={int(k):v for k,v in json.load(open(S+'bind12.json')).items()}
pat=re.compile(r'\]\[(\d+)\]:(.*)'); cur=None; L={}
for line in open('backups/applog/x/log1',errors='replace'):
    m=pat.search(line)
    if not m: continue
    ms=int(m.group(1))/1000; msg=m.group(2)
    mm=re.match(r'layer (\d+):',msg)
    if mm: cur=int(mm.group(1)); L[cur]={}; continue
    if cur is None: continue
    if 'axis 0 is idle' in msg and 'w0' not in L[cur]: L[cur]['w0']=ms
    if msg.startswith('printer start exposure'): L[cur]['exp']=ms
    if 'rest_time_after_drop' in msg: L[cur]['max']=float(re.search(r'rest_time_after_drop ([\d.]+)',msg).group(1))/1000
def grad(t,w0,win):
    a=bisect.bisect_left(T,t-win); b=bisect.bisect_right(T,t)
    if t-win<w0: return None
    xs=T[a:b]; ys=V[a:b]; n=len(xs)
    if n<3: return None
    mx=sum(xs)/n; my=sum(ys)/n; d=sum((x-mx)**2 for x in xs)
    return abs(sum((x-mx)*(y-my) for x,y in zip(xs,ys))/d) if d>0 else None
out=[]
for n,d in sorted(L.items()):
    if 'w0' not in d or 'exp' not in d or d['w0']<T[0]+1 or d['exp']>T[-1]: continue
    w0=d['w0']; mx=d['exp']-w0; t=w0; s=None
    while t<d['exp']:
        el=t-w0
        if el>=1.0:
            g=grad(t,w0,1.0)
            if g is not None and g<1.0: s=el; break
        t+=0.05
    csv=P12.get(n,{}).get('dyn')
    out.append((n,mx,s,csv))
for n,mx,s,c in out[::4]: print(n,'slicer %.1f'%mx,'C-rule %s'%('%.2f'%s if s else 'never'),'CSV-rule %s'%c)
a=[o for o in out if o[2]]; print('layers',len(out),'settled',len(a),'mean C %.2f s vs CSV %.2f s'%(st.mean(o[2] for o in a if o[3]),st.mean(o[3] for o in a if o[3])))
print('total wait slicer %.0f s, C-rule %.0f s'%(sum(o[1] for o in out),sum((o[2] or o[1]) for o in out)))
print('--- residual after trigger (g): max |g - g_at_trigger| until exposure; level change last 1 s')
bad=[]
for n,mx,s,c in out:
    if not s: continue
    w0=L[n]['w0']; ex=L[n]['exp']
    a=bisect.bisect_left(T,w0+s); b=bisect.bisect_left(T,ex)
    if b-a<2: continue
    ref=st.mean(V[max(a-2,0):a+1]); seg=V[a:b]
    res=max(abs(v-ref) for v in seg); fin=st.mean(V[b-5:b])-ref
    bad.append((res,n,s,fin))
bad.sort(reverse=True)
for r,n,s,f in bad[:10]: print('layer',n,'trigger %.2f s'%s,'max dev %.0f g'%r,'end level %+.0f g'%f)
print('median max dev %.0f g, median end shift %.0f g'%(st.median(b[0] for b in bad),st.median(abs(b[3]) for b in bad)))
