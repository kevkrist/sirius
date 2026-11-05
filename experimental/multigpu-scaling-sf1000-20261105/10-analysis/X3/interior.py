#!/usr/bin/env python3
"""interior.py <map.csv> <ngpu>: per query/switch: head (start->first H2D), interior link-idle (first->last H2D minus copy union), tail (last H2D->end)."""
import sqlite3, sys, csv
from collections import defaultdict
mapf, ng = sys.argv[1], int(sys.argv[2])
SW = [[0, 1], [2, 3]] if ng == 4 else [[0]]
def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out
tot = defaultdict(float)
print("q wall head interior tail copy (ms, max-switch)")
for r in csv.DictReader(open(mapf)):
    if r["iter"] != "2": continue
    w0, w1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    c = sqlite3.connect(r["sqlite"])
    cp = defaultdict(list)
    for s, e, d in c.execute("select start,end,deviceId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=? and srcKind=1 and dstKind=2 and bytes>=262144", (w0, w1)):
        cp[d].append((s, e))
    best = None
    for gs in SW:
        u = union([x for d in gs for x in cp[d]])
        if not u: continue
        cu = sum(e - s for s, e in u)
        v = dict(head=(u[0][0] - w0) / 1e6, interior=(u[-1][1] - u[0][0] - cu) / 1e6, tail=(w1 - u[-1][1]) / 1e6, copy=cu / 1e6)
        if best is None or v["copy"] + v["interior"] > best["copy"] + best["interior"]: best = v
    print(r["query"], round((w1 - w0) / 1e6), *[round(best[k]) for k in ("head", "interior", "tail", "copy")])
    for k in best: tot[k] += best[k]
    tot["wall"] += (w1 - w0) / 1e6
print({k: round(v) for k, v in tot.items()})
