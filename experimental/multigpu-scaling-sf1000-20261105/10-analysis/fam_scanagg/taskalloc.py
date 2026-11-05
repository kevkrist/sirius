#!/usr/bin/env python3
"""Per pipeline (NVTX 'Pipeline K Task M [...]' ranges): task count, summed task wall (thread-time), and the time inside
those ranges spent in driver allocator calls (cudaMallocFromPoolAsync + cudaFreeAsync), stream syncs, and other API.
usage: taskalloc.py <sqlite> <start_ns> <end_ns>"""
import sqlite3, sys, bisect, re
from collections import defaultdict
db, s0, e0 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
c = sqlite3.connect(db)
S = dict(c.execute("select id,value from StringIds"))
pat = re.compile(r"^Pipeline (\d+) Task")
rng = []
for s, e, tid, text, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (s0, e0)):
    n = text if text is not None else S.get(tx, "")
    m = pat.match(n)
    if m:
        rng.append((tid, s, e, int(m.group(1))))
tids = {r[0] for r in rng}
calls = defaultdict(list)
for s, e, tid, nid in c.execute("select start,end,globalTid,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (s0, e0)):
    if tid in tids:
        calls[tid].append((s, e, S.get(nid, '')))
for t in calls:
    calls[t].sort()
agg = defaultdict(lambda: defaultdict(float))
for tid, s, e, p in rng:
    a = agg[p]; a['n'] += 1; a['wall'] += (e - s) / 1e6
    cl = calls[tid]; ks = [x[0] for x in cl]
    for cs, ce, n in cl[bisect.bisect_left(ks, s):bisect.bisect_right(ks, e)]:
        d = (ce - cs) / 1e6
        if 'MallocFromPool' in n or 'FreeAsync' in n:
            a['alloc'] += d
        elif 'Synchronize' in n:
            a['sync'] += d
        else:
            a['other_api'] += d
print(f"window {(e0 - s0) / 1e6:.1f} ms")
for p in sorted(agg):
    a = agg[p]
    print(f"  P{p}: n={int(a['n'])} thread-ms {a['wall']:.0f} alloc {a['alloc']:.0f} ({100 * a['alloc'] / a['wall']:.0f}%) sync {a['sync']:.0f} other_api {a['other_api']:.0f}")
