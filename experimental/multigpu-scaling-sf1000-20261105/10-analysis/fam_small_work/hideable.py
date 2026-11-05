#!/usr/bin/env python3
"""Link-idle wall before the last HOST-scan upload starts (hideable by scan lookahead/prefetch) vs after (tail).
usage: hideable.py <quent_dir> <query> [iters]"""
import csv, sys
from collections import defaultdict
D, Q = sys.argv[1], sys.argv[2]; iters = [int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "2,3,4").split(",")]
qs = {r["query"]: r for r in csv.DictReader(open(f"{D}/queries.csv"))}
T = defaultdict(list)
for r in csv.DictReader(open(f"{D}/tasks.csv")):
    if r["query"].startswith(f"tpch_q{Q}_iter"): T[r["query"]].append(r)
pre = post = 0
for it in iters:
    lab = f"tpch_q{Q}_iter{it}"; q = qs[lab]; t0 = int(q["executing"]); n = (int(q["finished"]) - t0) // 10**6 + 1
    h = [False] * n; last = 0
    for r in T[lab]:
        if r["origin"] == "HOST" and r["preparing"] and r["first_compute"]:
            p = (int(r["preparing"]) - t0) // 10**6; f = (int(r["first_compute"]) - t0) // 10**6
            for i in range(max(p, 0), min(f + 1, n)): h[i] = True
    # last scan *pipeline* start: start of the last maximal run of upload activity >= 10 ms
    runs = []; s = None
    for i, v in enumerate(h + [False]):
        if v and s is None: s = i
        if not v and s is not None: runs.append((s, i)); s = None
    big = [r for r in runs if r[1] - r[0] >= 10]
    ls = big[-1][0] if big else 0; le = big[-1][1] if big else 0
    pre += sum(1 for i in range(ls) if not h[i]) / len(iters)
    post += sum(1 for i in range(le, n) if not h[i]) / len(iters)
print(f"Q{Q} {D.split('/')[-2] if D.endswith('quent') else D.split('/')[-1]}: link-idle before last big scan {pre:.1f} ms; after last scan (tail) {post:.1f} ms")
