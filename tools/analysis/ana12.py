import json,re,csv,statistics as st,math
S='/tmp/claude-1000/-home-dr-Claude-saturn4-fw/052dc525-3352-418d-b6ca-766ae2fd0911/scratchpad/'
rd=json.load(open(S+'cap12.json')); T=[t/1e6 for t,v in rd]; V=[v for t,v in rd]
K=100.252
ev={}; cur=None; layers={}
pat=re.compile(r'\]\[(\d+)\]:(.*)')
for line in open('backups/applog/x/log1',errors='replace'):
    m=pat.search(line)
    if not m: continue
    ms=int(m.group(1))/1000; msg=m.group(2)
    mm=re.match(r'layer (\d+):',msg)
    if mm: cur=int(mm.group(1)); layers[cur]={'start':ms}; continue
    if cur is None: continue
    d=layers[cur]
    if msg.startswith('printer start exposure'): d['exp']=ms
    elif msg.startswith('lifting = 1'): d['lift']=ms
    elif msg.startswith('lifting = 2'): d['lift2']=ms
    elif msg.startswith('lifting = 3'): d['end']=ms
    elif 'state:2,last' in msg: d['hon']=ms
    elif 'state:0,last:2' in msg: d['hoff']=ms
    elif 'rest_time_after_drop' in msg:
        d['wait']=float(re.search(r'rest_time_after_drop ([\d.]+)',msg).group(1))/1000
uv={int(r['layer_index']):r for r in csv.DictReader(open('UVTools/Piramidi 12x_peel_force.csv'))}
import bisect
res={}
for L,d in sorted(layers.items()):
    if 'lift' not in d or d['lift']<T[0]+2 or d['lift']>T[-1]-6: continue
    a=bisect.bisect_left(T,d['lift']-1.0); b=bisect.bisect_left(T,d['lift']+5)
    pre=[V[i] for i in range(bisect.bisect_left(T,d['lift']-3),a)]
    if len(pre)<3: continue
    lvl=st.median(pre)
    seg=[(T[i]-d['lift'],(V[i]-lvl)/K) for i in range(a,b)]
    j=min(range(len(seg)),key=lambda k:seg[k][1])
    res[L]=dict(lvl=lvl,seg=seg,peak=-seg[j][1],tpk=seg[j][0],area=float(uv[L]['area_mm2']) if L in uv else None,
                per=float(uv[L]['perimeter_mm']) if L in uv else None,**{k:d.get(k) for k in('exp','lift','lift2','end','hon','hoff','wait','start')})
json.dump(res,open(S+'res12.json','w'))
Ls=sorted(res); print('layers with capture',Ls[0],'-',Ls[-1],len(Ls))
for L in Ls[::6]:
    r=res[L]; print(L,'wait %.1f'%(r['wait'] or 0),'peak %.0f g at +%.2f s'%(r['peak'],r['tpk']),'area %.0f'%(r['area'] or 0),
      'tilt up %.2f s'%((r['lift2'] or 0)-r['lift']),'heat %s'%('%.2f-%.2f'%(r['hon']-r['lift'],r['hoff']-r['lift']) if r['hon'] and r['hoff'] else '-'))
