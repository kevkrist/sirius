#!/usr/bin/env python3
"""gap_staging.py <map.csv>: per query, busier switch: scan-copy gaps >=5 ms (as X3 interior_scan2) and how much of
that gap time has host-staged shuffle H2D (convert:gpu_to_gpu, pinned->device) in flight on the same switch, plus
staged bytes inside the gaps. Profiled ns -> ms."""
import sqlite3, sys, csv, bisect
from collections import defaultdict
mapf = sys.argv[1]; SW = [[0, 1], [2, 3]]
def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out
def overlap(u, a, b):
    return sum(max(0, min(e, b) - max(s, a)) for s, e in u)
T = defaultdict(float)
print("q gaps5_ms staged_busy_in_gaps_ms staged_GB_in_gaps(switch) staged_GB_total(switch)")
for r in csv.DictReader(open(mapf)):
    if r["iter"] != "2": continue
    w0, w1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    c = sqlite3.connect(r["sqlite"]); S = dict(c.execute("select id,value from StringIds"))
    g2g = defaultdict(list)
    for s, e, tid, t, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (w0, w1)):
        if (t if t is not None else S.get(tx, "")) == "convert:gpu_to_gpu": g2g[tid].append((s, e))
    gi = {t: (sorted(v), [x[0] for x in sorted(v)]) for t, v in g2g.items()}
    scan = defaultdict(list); stg = defaultdict(list)
    for s, e, d, tid, rs, b in c.execute("""select m.start,m.end,m.deviceId,r.globalTid,r.start,m.bytes from CUPTI_ACTIVITY_KIND_MEMCPY m join CUPTI_ACTIVITY_KIND_RUNTIME r on r.correlationId=m.correlationId
            where m.start>=? and m.end<=? and m.srcKind=1 and m.dstKind=2""", (w0, w1)):
        is_stg = False
        if tid in gi:
            v, st = gi[tid]; i = bisect.bisect_right(st, rs) - 1
            is_stg = i >= 0 and v[i][0] <= rs <= v[i][1]
        if is_stg: stg[d].append((s, e, b))
        elif b >= 262144: scan[d].append((s, e))
    best = None
    for gs in SW:
        u = union([x for d in gs for x in scan[d]])
        if not u: continue
        cu = sum(e - s for s, e in u)
        gaps = [(u[i][1], u[i + 1][0]) for i in range(len(u) - 1) if u[i + 1][0] - u[i][1] >= 5e6]
        su = union([(s, e) for d in gs for s, e, b in stg[d]])
        sb_in = sum(b for d in gs for s, e, b in stg[d] if any(a <= s < z for a, z in gaps))
        sb = sum(b for d in gs for s, e, b in stg[d])
        v = dict(gaps5=sum(b - a for a, b in gaps) / 1e6, busy=sum(overlap(su, a, b) for a, b in gaps) / 1e6, gb_in=sb_in / 1e9, gb=sb / 1e9, key=cu)
        if best is None or v['key'] > best['key']: best = v
    print(r["query"], round(best['gaps5']), round(best['busy']), round(best['gb_in'], 2), round(best['gb'], 2))
    for k in ('gaps5', 'busy', 'gb_in', 'gb'): T[k] += best[k]
print({k: round(v, 1) for k, v in T.items()})
