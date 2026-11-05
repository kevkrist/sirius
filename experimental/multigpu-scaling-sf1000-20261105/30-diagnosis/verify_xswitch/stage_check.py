#!/usr/bin/env python3
"""stage_check.py <map.csv> <queries comma> : per query (iter2 window), staged/peer clone legs:
per-switch uplink utilization during clone phases, concurrency of staged clones, leg serialization bubble."""
import sqlite3, sys, csv, bisect
from collections import defaultdict
mp=sys.argv[1]; QS=sys.argv[2].split(",")
SW=lambda d: 0 if d in (0,1) else 1
rows=[r for r in csv.DictReader(open(mp)) if r["iter"]=="2" and r["query"] in QS]
tot=defaultdict(float)
for r in rows:
    q=r["query"]; W0,W1=int(r["range_start_ns"]),int(r["range_end_ns"])
    c=sqlite3.connect(r["sqlite"]); S=dict(c.execute("select id,value from StringIds"))
    clones=[]  # (s,e,tid,kind)
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
    rt={}
    for cid,tid,s in c.execute("select correlationId,globalTid,start from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and start<=?",(W0,W1)):
        rt[cid]=(tid,s)
    mem=[]
    for s,e,dev,b,ck,sk,dk,sd,dd,cid in c.execute("select start,end,deviceId,bytes,copyKind,srcKind,dstKind,srcDeviceId,dstDeviceId,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?",(W0,W1)):
        tid,ts=rt.get(cid,(None,None)); idx=encl(tid,ts) if tid is not None else None
        kind=bytid[tid][idx][3] if idx is not None else "other"
        if sk==2 and dk in (0,1): d="D2H"
        elif sk in (0,1) and dk==2: d="H2D"
        elif sk==2 and dk==2: d="D2D" if sd==dd else "P2P"
        else: d=f"k{sk}{dk}"
        mem.append((s,e,dev,b,ck,d,sd,dd,kind,tid,idx))
    # clone phase union per query
    def union(iv):
        iv=sorted(iv); out=[]
        for s,e in iv:
            if out and s<=out[-1][1]: out[-1][1]=max(out[-1][1],e)
            else: out.append([s,e])
        return out
    stU=union([(s,e) for s,e,_,k in clones if k=="st"])
    stU_ms=sum(e-s for s,e in stU)/1e6
    # device of staged legs
    stleg=[m for m in mem if m[8]=="st"]
    devinfo=defaultdict(lambda:[0,0.0,0.0])
    for s,e,dev,b,ck,d,sd,dd,kind,tid,idx in stleg:
        key=(d,ck,"exec_on_src" if dev==sd else ("exec_on_dst" if dev==dd else f"dev{dev}"))
        devinfo[key][0]+=1; devinfo[key][1]+=b; devinfo[key][2]+=(e-s)
    print(f"== Q{q} window {(W1-W0)/1e6:.0f} ms; staged clones {sum(1 for x in clones if x[3]=='st')}, peer {sum(1 for x in clones if x[3]=='pe')}; staged-clone union {stU_ms:.0f} ms")
    for k,(n,b,t) in sorted(devinfo.items()):
        print(f"   staged leg {k}: n={n} {b/1e9:.2f} GB  mean {b/n/1e6:.2f} MB  per-copy rate {b/t:.1f} GB/s")
    # switch direction bytes. Staged D2H: source switch up; staged H2D dest switch down. Determine device for H2D=dev(dst), D2H src=sd
    BIN=5_000_000
    nb=(W1-W0)//BIN+1
    up=[[0.0]*nb for _ in range(2)]; dn=[[0.0]*nb for _ in range(2)]; p2p=[[0.0]*nb for _ in range(2)]
    def spread(arr,s,e,b):
        if e<=s: e=s+1
        i0=(s-W0)//BIN; i1=(e-W0)//BIN
        for i in range(i0,min(i1,nb-1)+1):
            a=max(s,W0+i*BIN); z=min(e,W0+(i+1)*BIN)
            if z>a: arr[i]+=b*(z-a)/(e-s)
    for s,e,dev,b,ck,d,sd,dd,kind,tid,idx in mem:
        if d=="D2H": spread(up[SW(sd if sd is not None else dev)],s,e,b)
        elif d=="H2D": spread(dn[SW(dd if dd is not None else dev)],s,e,b)
        elif d=="P2P": spread(p2p[SW(dev)],s,e,b)
    # staged-active bins: bins with staged clone in flight
    stbin=[0]*nb
    for s,e in stU:
        for i in range((s-W0)//BIN,min((e-W0)//BIN,nb-1)+1): stbin[i]=1
    act=[i for i in range(nb) if stbin[i]]
    if act:
        for sw in (0,1):
            u=sorted(up[sw][i]/BIN for i in act); dd_=sorted(dn[sw][i]/BIN for i in act)  # bytes/ns = GB/s
            mean=lambda L: sum(L)/len(L)
            print(f"   switch{'AB'[sw]} during staged bins ({len(act)}x5ms): up mean {mean(u):.1f} p90 {u[int(.9*len(u))]:.1f} GB/s | down mean {mean(dd_):.1f} p90 {dd_[int(.9*len(dd_))]:.1f} | p2p mean {mean([p2p[sw][i]/BIN for i in act]):.1f}")
    # concurrency: staged clones in flight per GPU (dest = thread's device unknown) -> overall count
    ev=sorted([(s,1) for s,e,_,k in clones if k=="st"]+[(e,-1) for s,e,_,k in clones if k=="st"])
    n=0;last=None;hist=defaultdict(float)
    for t,dl in ev:
        if last is not None and n>0: hist[n]+=t-last
        n+=dl; last=t
    totc=sum(hist.values())
    print("   staged clones in flight (time-weighted): mean %.1f; dist %s"%(sum(k*v for k,v in hist.items())/max(totc,1), {k:round(v/totc,2) for k,v in sorted(hist.items()) if v/totc>0.03}))
    # per-clone leg timeline: D2H span, gap, H2D span within each staged clone, and union of DMA vs clone duration
    per=defaultdict(list)
    for m in stleg: per[(m[9],m[10])].append(m)
    sumdur=sumd2h=sumh2d=sumgap=sumdmaU=0.0; nc=0
    for (tid,idx),L in per.items():
        cs,ce,_,_=bytid[tid][idx]; nc+=1; sumdur+=ce-cs
        d2=[(m[0],m[1]) for m in L if m[5]=="D2H"]; h2=[(m[0],m[1]) for m in L if m[5]=="H2D"]
        U=union(d2+h2); sumdmaU+=sum(e-s for s,e in U)
        sumd2h+=sum(e-s for s,e in union(d2)); sumh2d+=sum(e-s for s,e in union(h2))
    print(f"   per staged clone (n={nc}): dur {sumdur/nc/1e6:.3f} ms; D2H busy {sumd2h/nc/1e6:.3f}; H2D busy {sumh2d/nc/1e6:.3f}; DMA-busy union {sumdmaU/nc/1e6:.3f}; non-DMA {(sumdur-sumdmaU)/nc/1e6:.3f}")
    tot["dur"]+=sumdur; tot["d2h"]+=sumd2h; tot["h2d"]+=sumh2d; tot["U"]+=sumdmaU; tot["n"]+=nc; tot["stU"]+=stU_ms
print("TOTAL staged clones",tot["n"],"thread-s dur %.2f d2h %.2f h2d %.2f dmaU %.2f ; staged union wall %.0f ms"%(tot["dur"]/1e9,tot["d2h"]/1e9,tot["h2d"]/1e9,tot["U"]/1e9,tot["stU"]))
