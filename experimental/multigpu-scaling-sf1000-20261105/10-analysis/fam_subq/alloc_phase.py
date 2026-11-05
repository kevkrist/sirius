#!/usr/bin/env python3
"""Allocator calls (cudaMallocFromPoolAsync + cudaFreeAsync) per NVTX op-range name inside a query window,
plus phase rate (calls/s between first and last range start of that name).
usage: alloc_phase.py <map.csv> <query> <nvtx substr>..."""
import csv, sqlite3, sys, bisect
from collections import defaultdict
mp, q, subs = sys.argv[1], sys.argv[2], sys.argv[3:]
row = [r for r in csv.DictReader(open(mp)) if r['query'] == q and r['iter'] == '2'][0]
w0, w1 = int(row['range_start_ns']), int(row['range_end_ns'])
c = sqlite3.connect(row['sqlite']); S = dict(c.execute("select id,value from StringIds"))
ids = {i for i, v in S.items() if v.startswith('cudaMallocFromPoolAsync') or v.startswith('cudaFreeAsync')}
calls = defaultdict(list)
allc = []
for s, e, tid, n in c.execute("select start,end,globalTid,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (w0, w1)):
    if n in ids: calls[tid].append((s, e)); allc.append(s)
for t in calls: calls[t].sort()
allc.sort()
print(f"Q{q}: total alloc+free calls in window {len(allc)}, window {(w1-w0)/1e6:.0f} ms, mean rate {len(allc)/((w1-w0)/1e9)/1e3:.0f}k/s")
for sub in subs:
    rs = []
    for s, e, tid, text, tid2 in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (w0, w1)):
        nm = text if text is not None else S.get(tid2, '')
        if sub in nm and 'Task ' not in nm: rs.append((s, e, tid))
    if not rs: continue
    # dedupe nested identical ranges on same thread
    rs = sorted(set(rs))
    n = 0; d = 0; wall = 0
    last_end = {}
    for s, e, tid in rs:
        if tid in last_end and s < last_end[tid]: continue
        last_end[tid] = e; wall += e - s
        L = calls[tid]; i = bisect.bisect_left(L, (s, 0)); j = bisect.bisect_right(L, (e, 1 << 62))
        n += j - i; d += sum(x[1] - x[0] for x in L[i:j])
    p0 = min(r[0] for r in rs); p1 = max(r[1] for r in rs)
    inphase = bisect.bisect_right(allc, p1) - bisect.bisect_left(allc, p0)
    print(f"  {sub}: ranges={len(rs)} wall={wall/1e6:.0f}ms alloc_calls={n} ({n/len(rs):.0f}/range) alloc_ms={d/1e6:.0f} ({100*d/max(wall,1):.0f}% of wall); "
          f"phase {(p0-w0)/1e6:.0f}-{(p1-w0)/1e6:.0f}ms all-thread alloc calls in phase {inphase} -> {inphase/((p1-p0)/1e9)/1e3:.0f}k/s")
