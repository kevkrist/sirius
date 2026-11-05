#!/usr/bin/env python3
"""gap_fill.py <map.csv> <q>: for back-to-back staged D2H/H2D chunk gaps within a clone, fraction of gap time during which the
same device+direction engine executes ANY other memcpy (i.e. gap = engine time-slicing vs true idle overhead)."""
import sqlite3, sys, csv, bisect
from collections import defaultdict
mp,Q=sys.argv[1],sys.argv[2]
r=[r for r in csv.DictReader(open(mp)) if r["iter"]=="2" and r["query"]==Q][0]
W0,W1=int(r["range_start_ns"]),int(r["range_end_ns"])
c=sqlite3.connect(r["sqlite"]); S=dict(c.execute("select id,value from StringIds"))
cl=[]
for s,e,tid,t,tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?",(W0,W1)):
    nm=t if t is not None else S.get(tx,"")
    if nm=="convert:gpu_to_gpu:host_staging": cl.append((s,e,tid))
by=defaultdict(list)
for x in cl: by[x[2]].append(x)
for v in by.values(): v.sort()
st={k:[x[0] for x in v] for k,v in by.items()}
def enc(tid,t):
    i=bisect.bisect_right(st.get(tid,[]),t)-1
    return (tid,i) if i>=0 and by[tid][i][0]<=t<=by[tid][i][1] else None
rt={cid:(tid,s,e) for cid,tid,s,e in c.execute("select correlationId,globalTid,start,end from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and start<=?",(W0,W1))}
eng=defaultdict(list); per=defaultdict(list)
for s,e,dev,b,sk,dk,sd,dd,cid in c.execute("select start,end,deviceId,bytes,srcKind,dstKind,srcDeviceId,dstDeviceId,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?",(W0,W1)):
    d="d" if (sk==2 and dk!=2) else ("h" if (dk==2 and sk!=2) else "x")
    devx=(sd if d=="d" else dd) if d!="x" else dev; devx=dev if devx is None else devx
    eng[(devx,d)].append((s,e))
    x=rt.get(cid)
    if x:
        k=enc(x[0],x[1])
        if k: per[(k,d)].append((x[1],x[2],s,e,devx))
for v in eng.values(): v.sort()
starts={k:[a for a,_ in v] for k,v in eng.items()}
def busy(key,a,z):
    L=eng[key]; i=max(bisect.bisect_left(starts[key],a)-50,0); t=0
    while i<len(L) and L[i][0]<z:
        t+=max(0,min(z,L[i][1])-max(a,L[i][0])); i+=1
    return t
for d in ("d","h"):
    tot=filled=0; n=0
    for (k,dd_),L in per.items():
        if dd_!=d: continue
        L.sort()
        for i in range(1,len(L)):
            pe=L[i-1][3]; s=L[i][2]
            if s>pe and L[i][0]<pe:
                tot+=s-pe; filled+=busy((L[i][4],d),pe,s); n+=1
    print(f"Q{Q} {'D2H' if d=='d' else 'H2D'}: {n} back-to-back gaps, total {tot/1e6:.1f} ms, engine busy with other copies during gaps {filled/max(tot,1)*100:.0f}%")
