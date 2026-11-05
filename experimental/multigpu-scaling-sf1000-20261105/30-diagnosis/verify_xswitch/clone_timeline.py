#!/usr/bin/env python3
"""clone_timeline.py <map.csv> <queries>: per staged clone timeline segments + CUDA API time inside staged/peer clones
+ per-device copy-engine busy (union of memcpy by device & direction) during staged phases."""
import sqlite3, sys, csv, bisect
from collections import defaultdict, Counter
mp=sys.argv[1]; QS=sys.argv[2].split(",")
rows=[r for r in csv.DictReader(open(mp)) if r["iter"]=="2" and r["query"] in QS]
def union(iv):
    iv=sorted(iv); out=[]
    for s,e in iv:
        if out and s<=out[-1][1]: out[-1][1]=max(out[-1][1],e)
        else: out.append([s,e])
    return out
G=defaultdict(float); GA=defaultdict(lambda: defaultdict(float)); GC=defaultdict(lambda: defaultdict(int))
for r in rows:
    q=r["query"]; W0,W1=int(r["range_start_ns"]),int(r["range_end_ns"])
    c=sqlite3.connect(r["sqlite"]); S=dict(c.execute("select id,value from StringIds"))
    clones=[]
    for s,e,tid,t,tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?",(W0,W1)):
        nm=t if t is not None else S.get(tx,"")
        if nm=="convert:gpu_to_gpu:host_staging": clones.append((s,e,tid,"st"))
        elif nm=="convert:gpu_to_gpu:peer_copy": clones.append((s,e,tid,"pe"))
    bytid=defaultdict(list)
    for x in clones: bytid[x[2]].append(x)
    for v in bytid.values(): v.sort()
    def encl(tid,t):
        L=bytid.get(tid,[]); i=bisect.bisect_right(L,(t,1<<62,0,""))-1
        if i>=0 and L[i][0]<=t<=L[i][1]: return i
        return None
    rt={}; api=defaultdict(lambda: defaultdict(float)); apic=defaultdict(lambda: defaultdict(int))
    for cid,tid,s,e,nid in c.execute("select correlationId,globalTid,start,end,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and start<=?",(W0,W1)):
        rt[cid]=(tid,s,e)
        i=encl(tid,s)
        if i is not None:
            k=bytid[tid][i][3]; nm=S.get(nid,str(nid)).split("_v")[0]
            api[k][nm]+=e-s; apic[k][nm]+=1
    seg=defaultdict(float); n=0
    per=defaultdict(list)
    dev_busy=defaultdict(list)
    for s,e,dev,b,sk,dk,sd,dd,cid in c.execute("select start,end,deviceId,bytes,srcKind,dstKind,srcDeviceId,dstDeviceId,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?",(W0,W1)):
        x=rt.get(cid)
        if not x: continue
        tid,ts,te=x; i=encl(tid,ts)
        if i is None or bytid[tid][i][3]!="st": continue
        d="D2H" if sk==2 and dk!=2 else ("H2D" if dk==2 and sk!=2 else "o")
        per[(tid,i)].append((s,e,d,ts,te))
        dev_busy[(dev if (sd if d=="D2H" else dd) is None else (sd if d=="D2H" else dd),d)].append((s,e))
    for (tid,i),L in per.items():
        cs,ce,_,_=bytid[tid][i]
        d2=sorted([m for m in L if m[2]=="D2H"]); h2=sorted([m for m in L if m[2]=="H2D"])
        if not d2 or not h2: continue
        n+=1
        seg["start->firstD2Hissue"]+=d2[0][3]-cs
        seg["D2Hissue->firstD2Hexec"]+=d2[0][0]-d2[0][3]
        seg["D2H exec span"]+=max(m[1] for m in d2)-d2[0][0]
        seg["D2H busy (union)"]+=sum(e-s for s,e in union([(m[0],m[1]) for m in d2]))
        seg["lastD2H end->firstH2D exec"]+=h2[0][0]-max(m[1] for m in d2)
        seg["H2D exec span"]+=max(m[1] for m in h2)-h2[0][0]
        seg["H2D busy (union)"]+=sum(e-s for s,e in union([(m[0],m[1]) for m in h2]))
        seg["lastH2D end->clone end"]+=ce-max(m[1] for m in h2)
        seg["clone dur"]+=ce-cs
    print(f"== Q{q}: staged clones with legs {n}")
    for k,v in seg.items(): print(f"   {k:28s} {v/n/1e3:8.1f} us/clone  total {v/1e9:.3f} thread-s"); G[k]+=v
    G["n"]+=n
    for k in ("st","pe"):
        top=sorted(api[k].items(),key=lambda x:-x[1])[:8]
        print(f"   API in {k} clones: "+"; ".join(f"{a} {t/1e6:.0f}ms/{apic[k][a]} ({t/apic[k][a]/1e3:.0f}us)" for a,t in top))
        for a,t in api[k].items(): GA[k][a]+=t; GC[k][a]+=apic[k][a]
    # per-device copy engine busy fraction during staged union
    stU=union([(s,e) for s,e,_,k in clones if k=="st"]); stUms=sum(e-s for s,e in stU)
    for key in sorted(dev_busy):
        L=dev_busy[key]; U=union(L); ub=sum(e-s for s,e in U); sb=sum(e-s for s,e in L)
        print(f"   dev{key[0]} {key[1]} staged-leg busy-union {ub/1e6:.0f} ms (sum {sb/1e6:.0f} ms, mean concurrency {sb/max(ub,1):.2f}) of staged union {stUms/1e6:.0f} ms")
print("== ALL", int(G["n"]))
for k,v in G.items():
    if k!="n": print(f"   {k:28s} {v/G['n']/1e3:8.1f} us/clone  total {v/1e9:.3f} thread-s")
for k in ("st","pe"):
    top=sorted(GA[k].items(),key=lambda x:-x[1])[:10]
    print(f"   API {k}: "+"; ".join(f"{a} {t/1e9:.2f}s/{GC[k][a]} ({t/GC[k][a]/1e3:.0f}us)" for a,t in top))
