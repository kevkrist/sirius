#!/usr/bin/env python3
"""suite_floor.py <map.csv>: per query iter2: staged-clone union wall, peer-clone union, staged bytes per switch/direction,
uplink floor (pipelined, full duplex) and engine floor, kernel activity during staged union."""
import sqlite3, sys, csv, bisect
from collections import defaultdict
mp=sys.argv[1]
SW=lambda d: 0 if d in (0,1) else 1
UPCAP,DNCAP=57.0,52.0  # GB/s per switch uplink (microbench D2H/H2D)
def union(iv):
    iv=sorted(iv); out=[]
    for s,e in iv:
        if out and s<=out[-1][1]: out[-1][1]=max(out[-1][1],e)
        else: out.append([s,e])
    return out
def inter(A,B):
    i=j=0;t=0
    while i<len(A) and j<len(B):
        s=max(A[i][0],B[j][0]); e=min(A[i][1],B[j][1])
        if e>s: t+=e-s
        if A[i][1]<B[j][1]: i+=1
        else: j+=1
    return t
rows=[r for r in csv.DictReader(open(mp)) if r["iter"]=="2"]
rows.sort(key=lambda r:int(r["query"]))
T=defaultdict(float)
print("Q  win_ms stU_ms peU_ms anyU_ms st_GB  pe_GB  floor_up/dn_ms engfloor_ms stU&kern_ms stU&scanH2D_ms")
for r in rows:
    q=r["query"]; W0,W1=int(r["range_start_ns"]),int(r["range_end_ns"])
    c=sqlite3.connect(r["sqlite"]); S=dict(c.execute("select id,value from StringIds"))
    clones=[]
    for s,e,tid,t,tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?",(W0,W1)):
        nm=t if t is not None else S.get(tx,"")
        if nm=="convert:gpu_to_gpu:host_staging": clones.append((s,e,tid,"st"))
        elif nm=="convert:gpu_to_gpu:peer_copy": clones.append((s,e,tid,"pe"))
    if not clones:
        continue
    bytid=defaultdict(list)
    for x in clones: bytid[x[2]].append(x)
    for v in bytid.values(): v.sort()
    def encl(tid,t):
        L=bytid.get(tid,[]); i=bisect.bisect_right(L,(t,1<<62,0,""))-1
        if i>=0 and L[i][0]<=t<=L[i][1]: return L[i][3]
        return None
    rt={cid:(tid,s) for cid,tid,s in c.execute("select correlationId,globalTid,start from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and start<=?",(W0,W1))}
    up=[0.0,0.0]; dn=[0.0,0.0]; pe=0.0; st=0.0; eng=defaultdict(float); scanh2d=[]
    for s,e,dev,b,sk,dk,sd,dd,cid in c.execute("select start,end,deviceId,bytes,srcKind,dstKind,srcDeviceId,dstDeviceId,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?",(W0,W1)):
        x=rt.get(cid); k=encl(*x) if x else None
        if k=="st":
            if sk==2 and dk!=2: up[SW(sd if sd is not None else dev)]+=b; st+=b; eng[(sd if sd is not None else dev,"d")]+=e-s
            elif dk==2 and sk!=2: dn[SW(dd if dd is not None else dev)]+=b; eng[(dd if dd is not None else dev,"h")]+=e-s
        elif k=="pe": pe+=b
        elif k is None and dk==2 and sk in (0,1): scanh2d.append((s,e))
    stU=union([(s,e) for s,e,_,k in clones if k=="st"]); peU=union([(s,e) for s,e,_,k in clones if k=="pe"]); anyU=union([(s,e) for s,e,_,k in clones])
    kern=union([(s,e) for s,e in c.execute("select start,end from CUPTI_ACTIVITY_KIND_KERNEL where start>=? and end<=?",(W0,W1))])
    L=lambda U: sum(e-s for s,e in U)/1e6
    floor=max(max(up[i]/UPCAP,dn[i]/DNCAP) for i in (0,1))/1e6  # bytes/(GB/s) -> ns*... bytes/GBps = ns; /1e6 ms
    engf=max(eng.values())/1e6 if eng else 0
    sk_=inter(stU,kern)/1e6; sh=inter(stU,union(scanh2d))/1e6
    print(f"{q:>2} {(W1-W0)/1e6:7.0f} {L(stU):6.0f} {L(peU):6.0f} {L(anyU):6.0f} {st/1e9:6.2f} {pe/1e9:6.2f} {floor:8.0f} {engf:10.0f} {sk_:10.0f} {sh:10.0f}")
    for k,v in (("win",(W1-W0)/1e6),("stU",L(stU)),("peU",L(peU)),("anyU",L(anyU)),("st",st/1e9),("pe",pe/1e9),("floor",floor),("engf",engf),("stk",sk_),("sh",sh)): T[k]+=v
print("SUM", " ".join(f"{k}={v:.1f}" for k,v in T.items()))
