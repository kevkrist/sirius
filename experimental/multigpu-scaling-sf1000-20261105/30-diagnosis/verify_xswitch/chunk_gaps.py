#!/usr/bin/env python3
"""chunk_gaps.py <map.csv> <queries>: inside staged clones, gap between consecutive same-direction chunk copies of one clone
(exec end k -> exec start k+1) and issue->exec delay, D2H (foreign-stream, src device) vs H2D (own device); also peer clones."""
import sqlite3, sys, csv, bisect, statistics as stt
from collections import defaultdict
mp=sys.argv[1]; QS=sys.argv[2].split(",")
G=defaultdict(list); D=defaultdict(list)
for r in [r for r in csv.DictReader(open(mp)) if r["iter"]=="2" and r["query"] in QS]:
    W0,W1=int(r["range_start_ns"]),int(r["range_end_ns"])
    c=sqlite3.connect(r["sqlite"]); S=dict(c.execute("select id,value from StringIds"))
    cl=[]
    for s,e,tid,t,tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?",(W0,W1)):
        nm=t if t is not None else S.get(tx,"")
        if nm in("convert:gpu_to_gpu:host_staging","convert:gpu_to_gpu:peer_copy"): cl.append((s,e,tid,nm[-4:]))
    by=defaultdict(list)
    for x in cl: by[x[2]].append(x)
    for v in by.values(): v.sort()
    st={k:[x[0] for x in v] for k,v in by.items()}
    def enc(tid,t):
        i=bisect.bisect_right(st.get(tid,[]),t)-1
        return (tid,i) if i>=0 and by[tid][i][0]<=t<=by[tid][i][1] else None
    rt={cid:(tid,s,e) for cid,tid,s,e in c.execute("select correlationId,globalTid,start,end from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and start<=?",(W0,W1))}
    per=defaultdict(list)
    for s,e,b,sk,dk,cid in c.execute("select start,end,bytes,srcKind,dstKind,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?",(W0,W1)):
        x=rt.get(cid)
        if not x: continue
        k=enc(x[0],x[1])
        if not k: continue
        kind=by[k[0]][k[1]][3]
        d="P2P" if kind=="copy" else ("D2H" if sk==2 and dk!=2 else ("H2D" if dk==2 and sk!=2 else "o"))
        per[(k,d)].append((x[1],x[2],s,e,b))
    for (k,d),L in per.items():
        L.sort(key=lambda z:z[0])
        for i,(is_,ie,s,e,b) in enumerate(L):
            D[d].append((s-ie)/1e3)  # api return -> exec start (us)
            if i>0:
                pe=L[i-1][3]
                if s>=pe and is_<pe: G[d].append((s-pe)/1e3)  # chunk issued before previous finished -> pure device-side gap
def q(L,p): L=sorted(L); return L[int(p*(len(L)-1))]
for d in ("D2H","H2D","P2P"):
    if G[d]: print(f"{d}: back-to-back chunk exec gap (issued before prev done) n={len(G[d])} median {q(G[d],.5):.1f} us p90 {q(G[d],.9):.1f} mean {sum(G[d])/len(G[d]):.1f}")
    if D[d]: print(f"{d}: api-return->exec-start n={len(D[d])} median {q(D[d],.5):.1f} us p90 {q(D[d],.9):.1f} mean {sum(D[d])/len(D[d]):.1f}")
