#!/usr/bin/env python3
"""nsys_timeline.py <map.csv> <query> : pipeline spans (from 'Pipeline K Task' NVTX), dynfilter ranges, per-GPU kernel busy and H2D bytes per pipeline span (iter2 window)."""
import sqlite3,re,csv,sys
from collections import defaultdict
mp,q=sys.argv[1],sys.argv[2]
r=[x for x in csv.DictReader(open(mp)) if x['query']==q and x['iter']=='2'][0]
W0,W1=int(r['range_start_ns']),int(r['range_end_ns'])
c=sqlite3.connect(r['sqlite']); S=dict(c.execute("select id,value from StringIds"))
sp=defaultdict(lambda:[0,1e30,0,0,'']); df=[]
for s,e,t,tid in c.execute("select start,end,text,textId from NVTX_EVENTS where eventType=59 and start>=? and end<=?",(W0,W1)):
    n=t if t is not None else S.get(tid,"")
    m=re.match(r'Pipeline (\d+) Task \d+ \[(.*)\]',n)
    if m:
        a=sp[int(m.group(1))]; a[0]+=1; a[1]=min(a[1],s-W0); a[2]=max(a[2],e-W0); a[3]+=e-s; a[4]=m.group(2)[:60]
    elif n.startswith('dynfilter::accum::finish') or n.startswith('dynfilter::publish_hook'):
        df.append((n,(s-W0)/1e6,(e-s)/1e6))
# kernel busy per device per span (union approx by sum, capped)
K=list(c.execute("select deviceId,start,end from CUPTI_ACTIVITY_KIND_KERNEL where start>=? and end<=?",(W0,W1)))
M=list(c.execute("select deviceId,start,end,bytes,srcKind,dstKind from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?",(W0,W1)))
def union(iv):
    iv.sort(); tot=0; cs=ce=None
    for s,e in iv:
        if cs is None or s>ce:
            if cs is not None: tot+=ce-cs
            cs,ce=s,e
        else: ce=max(ce,e)
    if cs is not None: tot+=ce-cs
    return tot
print(f"Q{q} window {(W1-W0)/1e6:.1f} ms  report {r['sqlite'].split('/')[-1]}")
print(f"{'P':>3} {'n':>4} {'start':>7} {'end':>7} {'span':>6} {'thr':>7} {'kbusy/dev(ms)':>26} {'H2Dpinned GB':>12} {'GB/s':>6}  label")
for k in sorted(sp,key=lambda k:sp[k][1]):
    a=sp[k]; s,e=a[1]+W0,a[2]+W0
    kb=[union([(max(x[1],s),min(x[2],e)) for x in K if x[0]==d and x[2]>s and x[1]<e])/1e6 for d in range(4)]
    h=sum(x[3] for x in M if x[4]==1 and x[5]==2 and x[1]>=s and x[2]<=e)/1e9
    print(f"{k:>3} {a[0]:>4} {a[1]/1e6:7.1f} {a[2]/1e6:7.1f} {(a[2]-a[1])/1e6:6.1f} {a[3]/1e6:7.1f} {' '.join('%6.1f'%x for x in kb):>26} {h:12.2f} {h/max((e-s)/1e9,1e-9):6.1f}  {a[4]}")
for d in df: print('  ',d[0],'at %.1f dur %.1f'%(d[1],d[2]))
