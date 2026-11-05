#!/usr/bin/env python3
"""nsys_qsum.py <sqlite> <start_ns> <end_ns> : whole-query nsys summary within window.
Per device: kernel union-busy ms, kernel sum ms, memcpy bytes/ms by kind; top kernels; runtime API totals; NVTX op ranges."""
import sqlite3, sys
from collections import Counter, defaultdict
db, W0, W1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
c = sqlite3.connect(db)
S = dict(c.execute("select id,value from StringIds"))
def union(iv):
    iv.sort(); tot = 0; cs = ce = None
    for s, e in iv:
        if cs is None or s > ce:
            if cs is not None: tot += ce - cs
            cs, ce = s, e
        else: ce = max(ce, e)
    if cs is not None: tot += ce - cs
    return tot
kern = defaultdict(list); ksum = Counter(); kname = Counter(); kcnt = Counter()
for s, e, d, n in c.execute("select start,end,deviceId,shortName from CUPTI_ACTIVITY_KIND_KERNEL where start>=? and end<=?", (W0, W1)):
    kern[d].append((s, e)); ksum[d] += e - s; kname[S.get(n, n)] += e - s; kcnt[S.get(n, n)] += 1
print(f"window {(W1-W0)/1e6:.1f} ms")
for d in sorted(kern):
    print(f"dev{d}: kernel busy(union) {union(kern[d])/1e6:8.1f} ms  kernel sum {ksum[d]/1e6:8.1f} ms  n={len(kern[d])}")
KIND = {1: 'HtoD', 2: 'DtoH', 8: 'DtoD', 10: 'PtoP'}
mb = Counter(); mt = Counter(); mn = Counter(); mbusy = defaultdict(list)
for s, e, d, k, b, sk, dk in c.execute("select start,end,deviceId,copyKind,bytes,srcKind,dstKind from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?", (W0, W1)):
    key = (d, KIND.get(k, k)); mb[key] += b; mt[key] += e - s; mn[key] += 1; mbusy[key].append((s, e))
for key in sorted(mb):
    u = union(mbusy[key])
    print(f"dev{key[0]} {key[1]:5}: {mb[key]/1e9:8.2f} GB  n={mn[key]:7}  sum {mt[key]/1e6:8.1f} ms  busy {u/1e6:8.1f} ms  {mb[key]/max(u,1):6.1f} GB/s(busy)")
print("top kernels (sum ms, count):")
for n, v in kname.most_common(12): print(f"  {v/1e6:9.1f} {kcnt[n]:7}  {n[:90]}")
api = Counter(); apin = Counter()
for s, e, n in c.execute("select start,end,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (W0, W1)):
    api[S.get(n, n)] += e - s; apin[S.get(n, n)] += 1
print("top runtime APIs (thread-ms, count):")
for n, v in api.most_common(14): print(f"  {v/1e6:9.1f} {apin[n]:8}  {n}")
nv = Counter(); nvn = Counter()
for s, e, t, tid in c.execute("select start,end,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (W0, W1)):
    name = t if t is not None else S.get(tid, '')
    if name.startswith('Pipeline') and 'Task' in name: name = 'Pipeline*Task ' + name.split(']')[-1][:40] if ']' in name else 'Pipeline*Task'
    nv[name] += e - s; nvn[name] += 1
print("top NVTX ranges (thread-ms, count):")
for n, v in nv.most_common(40): print(f"  {v/1e6:9.1f} {nvn[n]:8}  {n[:100]}")
