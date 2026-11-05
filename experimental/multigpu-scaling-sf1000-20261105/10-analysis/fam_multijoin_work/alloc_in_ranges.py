#!/usr/bin/env python3
"""alloc_in_ranges.py <map.csv> <query> <nvtx substring>: inside matching NVTX ranges (same thread, query iter2 window):
count/total/mean of cudaMallocFromPoolAsync, cudaFreeAsync, cudaStreamSynchronize, cuStreamSynchronize, cudaMemcpyAsync."""
import sqlite3, csv, sys, collections, bisect
mp, q, sub = sys.argv[1], int(sys.argv[2]), sys.argv[3]
row = next(r for r in csv.DictReader(open(mp)) if int(r['query']) == q and r['iter'] == '2')
s, e = int(row['range_start_ns']), int(row['range_end_ns']); c = sqlite3.connect(row['sqlite'])
S = dict(c.execute("select id,value from StringIds"))
rng = collections.defaultdict(list)
for st, en, tid, text, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (s, e)):
    n = text if text is not None else S.get(tx, '')
    if sub in n: rng[tid].append((st, en))
for t in rng: rng[t].sort()
wall = sum(en - st for L in rng.values() for st, en in L) / 1e6
agg = collections.defaultdict(lambda: [0, 0])
for st, en, tid, nid in c.execute("select start,end,globalTid,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (s, e)):
    L = rng.get(tid)
    if not L: continue
    i = bisect.bisect_right(L, (st, 1 << 62)) - 1
    if i >= 0 and L[i][0] <= st and en <= L[i][1]:
        nm = S[nid].split('_v')[0]; agg[nm][0] += 1; agg[nm][1] += en - st
n = sum(len(L) for L in rng.values())
print(f"Q{q} '{sub}': ranges={n} wall={wall:.0f}ms")
for k, (cnt, ns) in sorted(agg.items(), key=lambda kv: -kv[1][1])[:8]:
    print(f"   {k:28} n={cnt:7} total={ns/1e6:8.1f}ms mean={ns/cnt/1e3:6.1f}us  per-range={cnt/n:6.1f}")
