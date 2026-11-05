#!/usr/bin/env python3
"""burst_util.py <map.csv> <q>: 1ms bins; per-switch staged up/down GB/s and per-device staged-engine busy, bucketed by # staged clones in flight."""
import sqlite3, sys, csv, bisect
from collections import defaultdict
mp,Q=sys.argv[1],sys.argv[2]
r=[r for r in csv.DictReader(open(mp)) if r["iter"]=="2" and r["query"]==Q][0]
W0,W1=int(r["range_start_ns"]),int(r["range_end_ns"]); BIN=1_000_000; nb=(W1-W0)//BIN+1
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
    return i>=0 and by[tid][i][0]<=t<=by[tid][i][1]
rt={cid:(tid,s) for cid,tid,s in c.execute("select correlationId,globalTid,start from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and start<=?",(W0,W1))}
inflight=[0.0]*nb
for s,e,_ in cl:
    for i in range((s-W0)//BIN,min((e-W0)//BIN,nb-1)+1):
        a=max(s,W0+i*BIN); z=min(e,W0+(i+1)*BIN); inflight[i]+=(z-a)/BIN
busy=defaultdict(lambda:[0.0]*nb); sw=defaultdict(lambda:[0.0]*nb)
SW=lambda d: 0 if d in (0,1) else 1
for s,e,dev,b,sk,dk,sd,dd,cid in c.execute("select start,end,deviceId,bytes,srcKind,dstKind,srcDeviceId,dstDeviceId,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?",(W0,W1)):
    x=rt.get(cid)
    if not x or not enc(*x): continue
    d="d" if (sk==2 and dk!=2) else ("h" if dk==2 and sk!=2 else None)
    if d is None: continue
    devx=(sd if d=="d" else dd); devx=dev if devx is None else devx
    for i in range((s-W0)//BIN,min((e-W0)//BIN,nb-1)+1):
        a=max(s,W0+i*BIN); z=min(e,W0+(i+1)*BIN)
        if z>a: busy[(devx,d)][i]+=(z-a)/BIN; sw[(SW(devx),d)][i]+=b*(z-a)/(e-s)/BIN
groups=[(0.5,4),(4,8),(8,12),(12,99)]
for lo,hi in groups:
    idx=[i for i in range(nb) if lo<=inflight[i]<hi]
    if not idx: continue
    eb=sum(sum(busy[k][i] for i in idx)/len(idx) for k in busy)/len(busy)
    emax=max(sum(busy[k][i] for i in idx)/len(idx) for k in busy)
    upd=[sum(sw[(s_,d)][i] for i in idx)/len(idx) for s_ in (0,1) for d in ("d","h")]
    print(f"Q{Q} inflight [{lo},{hi}): {len(idx)} ms; mean engine busy {eb:.2f} (max dev {emax:.2f}); per-switch staged GB/s up A {upd[0]:.1f} dn A {upd[1]:.1f} up B {upd[2]:.1f} dn B {upd[3]:.1f}")
