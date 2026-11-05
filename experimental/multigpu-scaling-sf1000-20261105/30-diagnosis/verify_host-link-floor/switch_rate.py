#!/usr/bin/env python3
"""Independent per-switch H2D rate check: host(pinned/pageable)->device copies inside the query window,
union of intervals per switch, bytes/union; plus 10ms-bin peak rate. Usage: switch_rate.py <sqlite> <start> <end>"""
import sqlite3,sys
db,s,e=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
c=sqlite3.connect(db)
rows=c.execute("""select start,end,bytes,deviceId,copyKind,srcKind,dstKind from CUPTI_ACTIVITY_KIND_MEMCPY
 where start>=? and end<=? and dstKind=2 and srcKind in (0,1) """,(s,e)).fetchall()
def union(iv):
    iv=sorted(iv);tot=0;cs=ce=None
    for a,b in iv:
        if cs is None or a>ce:
            if cs is not None: tot+=ce-cs
            cs,ce=a,b
        else: ce=max(ce,b)
    if cs is not None: tot+=ce-cs
    return tot
for name,devs in (('swA',(0,1)),('swB',(2,3)),('all',(0,1,2,3))):
    r=[x for x in rows if x[3] in devs]
    B=sum(x[2] for x in r); U=union([(x[0],x[1]) for x in r])
    # binned rate (bytes apportioned by overlap) in 20 ms bins
    W=20_000_000; nb=(e-s)//W+1; bins=[0.0]*nb
    for a,b,by,*_ in r:
        d=b-a or 1
        k=(a-s)//W
        while a<b:
            bend=min(b,s+(k+1)*W); bins[k]+=by*(bend-a)/d; a=bend; k+=1
    pk=sorted(bins)[-3:]
    print(f"{name}: {B/1e9:.1f} GB  union {U/1e6:.0f} ms  rate {B/U:.1f} GB/s  window {(e-s)/1e6:.0f} ms  busy {U/(e-s)*100:.0f}%  top20ms-bins GB/s {[round(x/W,1) for x in pk]}  n={len(r)}")
