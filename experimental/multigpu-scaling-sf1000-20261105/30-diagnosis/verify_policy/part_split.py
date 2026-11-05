#!/usr/bin/env python3
"""part_split.py <map.csv> <query> : per 'Pipeline K: PARTITION' NVTX op-range group in the iter2 window:
n ranges, thread-ms, alloc api ms (cudaMallocFromPoolAsync+cudaFreeAsync, same thread, inside range),
non-alloc ms, phase span ms (first start..last end), distinct threads, max per-thread non-alloc ms,
and kernel ms launched. Counterfactual 'no contended allocator' span lower bound = max(per-thread non-alloc + per-thread alloc*g1/g4 ratio)."""
import sqlite3, csv, sys, re, bisect
from collections import defaultdict
mp, q = sys.argv[1], sys.argv[2]
row = [r for r in csv.DictReader(open(mp)) if r['iter']=='2' and r['query']==q][0]
W0, W1 = int(row['range_start_ns']), int(row['range_end_ns'])
c = sqlite3.connect(row['sqlite']); S = dict(c.execute("select id,value from StringIds"))
rng = defaultdict(list)
for st, en, tid, text, tidx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (W0, W1)):
    name = text if text is not None else S.get(tidx, '')
    m = re.match(r'Pipeline (\d+): PARTITION', name)
    if m: rng[name].append((tid, st, en))
tids = {t for v in rng.values() for t,_,_ in v}
api = defaultdict(list)
alloc_ids = {i for i,v in S.items() if v.startswith('cudaMallocFromPoolAsync') or v.startswith('cudaFreeAsync')}
for st, en, tid, nid, corr in c.execute("select start,end,globalTid,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (W0, W1)):
    if tid in tids: api[tid].append((st, en, nid in alloc_ids, corr))
for t in api: api[t].sort()
kern = dict(c.execute("select correlationId, end-start from CUPTI_ACTIVITY_KIND_KERNEL"))
print(f"Q{q} window {(W1-W0)/1e6:.1f} ms")
print(f"{'range':40}{'n':>6}{'thr_ms':>9}{'alloc':>8}{'nonal':>8}{'span':>8}{'nthr':>5}{'maxthr_nonal':>13}{'kern':>7}")
for name, v in sorted(rng.items(), key=lambda kv: -sum(e-s for _,s,e in kv[1])):
    T = sum(e-s for _,s,e in v)/1e6
    A = 0; K = 0; per = defaultdict(float)
    for tid, s, e in v:
        L = api[tid]; i = bisect.bisect_left(L, (s,))
        a = 0
        while i < len(L) and L[i][0] < e:
            if L[i][2]: a += L[i][1]-L[i][0]
            K += kern.get(L[i][3], 0); i += 1
        A += a; per[tid] += (e-s-a)/1e6
    A /= 1e6
    span = (max(e for _,_,e in v)-min(s for _,s,_ in v))/1e6
    print(f"{name[:40]:40}{len(v):>6}{T:>9.1f}{A:>8.1f}{T-A:>8.1f}{span:>8.1f}{len(per):>5}{max(per.values()):>13.1f}{K/1e6:>7.1f}")
