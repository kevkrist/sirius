#!/usr/bin/env python3
"""clone_regress.py <map.csv> : per clone (staged/peer) duration vs bytes; OLS fit dur = a + b*GB per kind; byte-size buckets."""
import sqlite3, sys, csv, bisect
from collections import defaultdict
mp=sys.argv[1]
rows=[r for r in csv.DictReader(open(mp)) if r["iter"]=="2"]
pts={"st":[], "pe":[]}
for r in rows:
    W0,W1=int(r["range_start_ns"]),int(r["range_end_ns"])
    c=sqlite3.connect(r["sqlite"]); S=dict(c.execute("select id,value from StringIds"))
    clones=[]
    for s,e,tid,t,tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?",(W0,W1)):
        nm=t if t is not None else S.get(tx,"")
        if nm=="convert:gpu_to_gpu:host_staging": clones.append([s,e,tid,"st",0,0])
        elif nm=="convert:gpu_to_gpu:peer_copy": clones.append([s,e,tid,"pe",0,0])
    if not clones: continue
    bytid=defaultdict(list)
    for x in clones: bytid[x[2]].append(x)
    for v in bytid.values(): v.sort(key=lambda x:x[0])
    starts={k:[x[0] for x in v] for k,v in bytid.items()}
    def encl(tid,t):
        L=bytid.get(tid,[]); i=bisect.bisect_right(starts.get(tid,[]),t)-1
        if i>=0 and L[i][0]<=t<=L[i][1]: return L[i]
        return None
    rt={cid:(tid,s) for cid,tid,s in c.execute("select correlationId,globalTid,start from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and start<=?",(W0,W1))}
    for s,e,b,sk,dk,cid in c.execute("select start,end,bytes,srcKind,dstKind,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?",(W0,W1)):
        x=rt.get(cid); cl=encl(*x) if x else None
        if cl is None: continue
        if cl[3]=="st":
            if sk==2 and dk!=2: cl[4]+=b; cl[5]+=e-s
            elif dk==2 and sk!=2: cl[5]+=e-s
        else:
            cl[4]+=b; cl[5]+=e-s
    for x in clones: pts[x[3]].append(((x[1]-x[0])/1e6, x[4]/1e9, x[5]/1e6))
for k,P in pts.items():
    n=len(P); sx=sum(p[1] for p in P); sy=sum(p[0] for p in P)
    mx=sx/n; my=sy/n
    b=sum((p[1]-mx)*(p[0]-my) for p in P)/sum((p[1]-mx)**2 for p in P); a=my-b*mx
    dma=sum(p[2] for p in P)
    print(f"{k}: n={n} GB={sx:.1f} thread-s={sy/1e3:.2f} DMA thread-s={dma/1e3:.2f} | fit dur_ms = {a:.3f} + {b:.1f}*GB  => fixed part {a*n/1e3:.2f} s, byte part {b*sx/1e3:.2f} s (eff {1e3/b:.1f} GB/s per flow)")
    bk=defaultdict(lambda:[0,0.0,0.0,0.0])
    for d,g,m in P:
        key=0 if g<0.0005 else (1 if g<0.002 else (2 if g<0.008 else (3 if g<0.032 else 4)))
        bk[key][0]+=1; bk[key][1]+=d; bk[key][2]+=g; bk[key][3]+=m
    for key in sorted(bk):
        nn,d,g,m=bk[key]; print(f"   bucket {['<0.5MB','0.5-2MB','2-8MB','8-32MB','>32MB'][key]}: n={nn} mean dur {d/nn:.3f} ms, mean {g/nn*1e3:.2f} MB, DMA {m/nn:.3f} ms, eff {g/d*1e3 if d else 0:.1f} GB/s, total {d/1e3:.2f} s")
