import json,math,statistics as st,sys,csv
S='/tmp/claude-1000/-home-dr-Claude-saturn4-fw/052dc525-3352-418d-b6ca-766ae2fd0911/scratchpad/'
sys.argv=['x',S]; src=open(S+'bind.py').read().split('A=analyse')[0]; exec(src)
P12=analyse(load('backups/sglog/sglog/20261019_211148_Piramidi_12x.csv'))
P3=json.load(open(S+'bind.json'))['A']; P3={int(k):v for k,v in P3.items()}
U12={int(x['layer_index']):x for x in load('UVTools/Piramidi 12x_peel_force.csv')}
U3={int(x['layer_index']):x for x in load('UVTools/Piramidi 3x_peel_force.csv')}
json.dump(P12,open(S+'bind12.json','w'))
def side(U,L,n): return math.sqrt(float(U[L]['area_mm2'])/n)
print('SETTLING (gradient <1 g/s for 1 s) by island side')
bins=[(48,56),(44,48),(40,44),(36,40),(32,36),(28,32),(24,28),(20,24),(16,20),(12,16),(8,12),(4,8),(0,4)]
for lo,hi in bins:
    row=[]
    for P,U,n in ((P3,U3,3),(P12,U12,12)):
        v=[P[L] for L in P if L in U and L>=2 and lo<=side(U,L,n)<hi]
        if not v: row.append('            -        '); continue
        d=[x['dyn'] for x in v if x['dyn'] is not None]; nv=len(v)-len(d)
        row.append('n%3d %5s s never %2d'%(len(v),('%.1f'%st.median(d)) if d else '-',nv))
    print('%2d-%2d mm | 3x %s | 12x %s'%(lo,hi,row[0],row[1]))
print()
print('12x per layer: L side wait_used settle peel level')
for L in sorted(P12):
    if L in U12 and (L<20 or L%10==0):
        x=P12[L]; print(L,'%.1f'%side(U12,L,12),'%.1f'%x['rest'],x['dyn'] and round(x['dyn'],1),round(x['peel']),round(x['lvl']))
# peel fit, both prints, F0 + kA*A + kP*P per speed range
def lsq(X,y):
    n=len(X[0]); M=[[sum(a[i]*a[j] for a in X) for j in range(n)] for i in range(n)]; b=[sum(a[i]*c for a,c in zip(X,y)) for i in range(n)]
    for i in range(n):
        p=M[i][i]
        for j in range(i+1,n):
            f=M[j][i]/p; M[j]=[a-f*c for a,c in zip(M[j],M[i])]; b[j]-=f*b[i]
    x=[0]*n
    for i in reversed(range(n)): x[i]=(b[i]-sum(M[i][j]*x[j] for j in range(i+1,n)))/M[i][i]
    r=[c-sum(a*q for a,q in zip(row,x)) for row,c in zip(X,y)]; my=st.mean(y)
    return x, math.sqrt(st.mean([e*e for e in r])), 1-sum(e*e for e in r)/sum((c-my)**2 for c in y)
print()
for name,rng in (('slow 15-49',range(15,50)),('fast 50+',range(50,300))):
    X=[];y=[];tag=[]
    for P,U,t in ((P3,U3,'3x'),(P12,U12,'12x')):
        for L in rng:
            if L in P and L in U and float(U[L]['area_mm2'])>100:
                X.append([1,float(U[L]['area_mm2']),float(U[L]['perimeter_mm'])]); y.append(P[L]['peel']); tag.append(t)
    for sub in ('3x','12x'):
        Xs=[[a[0],a[1]] for a,tt in zip(X,tag) if tt==sub]; ys=[c for c,tt in zip(y,tag) if tt==sub]
        c,rms,r2=lsq(Xs,ys); print(name,sub,'area only: F0 %.1f N kA %.4f N/mm2 rms %.0f g R2 %.2f n %d'%(c[0]*.00981,c[1]*.00981,rms,r2,len(ys)))
    c,rms,r2=lsq(X,y); print(name,'BOTH area+perimeter: F0 %.1f N kA %.4f N/mm2 kP %.4f N/mm rms %.0f g R2 %.2f'%(c[0]*.00981,c[1]*.00981,c[2]*.00981,rms,r2))
    c,rms,r2=lsq([a[:2] for a in X],y); print(name,'BOTH area only: F0 %.1f N kA %.4f rms %.0f R2 %.2f'%(c[0]*.00981,c[1]*.00981,rms,r2))
