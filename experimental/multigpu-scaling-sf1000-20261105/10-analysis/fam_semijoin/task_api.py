#!/usr/bin/env python3
"""task_api.py <map.csv> <query>: per pipeline, sum of 'Pipeline K Task' NVTX range wall (thread-ms) and the CUDA API time inside
(malloc=cudaMallocFromPoolAsync, free=cudaFreeAsync, sync=cuda/cuStreamSynchronize, cpy=cudaMemcpy*Async), counts, launched kernel ms."""
import sqlite3,re,csv,sys,bisect
from collections import defaultdict,Counter
mp,q=sys.argv[1],sys.argv[2]
r=[x for x in csv.DictReader(open(mp)) if x['query']==q and x['iter']=='2'][0]
W0,W1=int(r['range_start_ns']),int(r['range_end_ns'])
c=sqlite3.connect(r['sqlite']); S=dict(c.execute("select id,value from StringIds"))
rng=defaultdict(list); lab={}
for s,e,t,tid,gt in c.execute("select start,end,text,textId,globalTid from NVTX_EVENTS where eventType=59 and start>=? and end<=?",(W0,W1)):
    n=t if t is not None else S.get(tid,"")
    m=re.match(r'Pipeline (\d+) Task \d+ \[(.*)\]',n)
    if m: rng[gt].append((s,e,int(m.group(1)))); lab[int(m.group(1))]=m.group(2)[:50]
kern={corr:e-s for s,e,corr in c.execute("select start,end,correlationId from CUPTI_ACTIVITY_KIND_KERNEL where start>=? and end<=?",(W0,W1))}
api=defaultdict(list)
for s,e,nid,gt,corr in c.execute("select start,end,nameId,globalTid,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?",(W0,W1)):
    if gt in rng: api[gt].append((s,e,S[nid].split('_v')[0],corr))
def cls(n):
    if n=='cudaMallocFromPoolAsync': return 'malloc'
    if n=='cudaFreeAsync': return 'free'
    if 'Synchronize' in n: return 'sync'
    if n.startswith('cudaMemcpy'): return 'cpy'
    return 'other'
A=defaultdict(Counter); N=defaultdict(Counter)
for gt,rs in rng.items():
    a=sorted(api[gt]); st=[x[0] for x in a]
    for s,e,p in rs:
        A[p]['wall']+=e-s; N[p]['tasks']+=1
        for x in a[bisect.bisect_left(st,s):bisect.bisect_right(st,e)]:
            k=cls(x[2]); A[p][k]+=x[1]-x[0]; N[p][k]+=1
            if x[3] in kern: A[p]['kern']+=kern[x[3]]
print(f"Q{q} {mp.split('/')[-3]}  (thread-ms)")
print(f"{'P':>3} {'tasks':>5} {'wall':>8} {'malloc':>7} {'free':>7} {'sync':>7} {'cpyAPI':>7} {'other':>6} {'kern':>6} {'nMal':>6} {'usMal':>6} {'usFree':>6}  label")
for p in sorted(A):
    a=A[p]; n=N[p]
    print(f"{p:>3} {n['tasks']:>5} {a['wall']/1e6:8.1f} {a['malloc']/1e6:7.1f} {a['free']/1e6:7.1f} {a['sync']/1e6:7.1f} {a['cpy']/1e6:7.1f} {a['other']/1e6:6.1f} {a['kern']/1e6:6.1f} {n['malloc']:>6} {a['malloc']/max(n['malloc'],1)/1e3:6.1f} {a['free']/max(n['free'],1)/1e3:6.1f}  {lab[p]}")
t=Counter()
for p in A: t.update(A[p])
print(f"TOT       {t['wall']/1e6:8.1f} {t['malloc']/1e6:7.1f} {t['free']/1e6:7.1f} {t['sync']/1e6:7.1f} {t['cpy']/1e6:7.1f} {t['other']/1e6:6.1f} {t['kern']/1e6:6.1f}")
