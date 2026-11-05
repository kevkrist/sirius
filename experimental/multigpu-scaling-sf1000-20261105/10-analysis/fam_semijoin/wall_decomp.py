#!/usr/bin/env python3
"""wall_decomp.py <quent_dir> <qnum> [iters]: attribute each 0.1ms of query wall to one category by priority
(a bin goes to the highest-priority category with a task in flight [preparing..finalize]).
Categories: SCAN (pipeline with GPU_SCAN and HOST-origin tasks), PARTITION, CONCAT, MERGE (MERGE_*), COMPUTE (join/groupby/other), IDLE.
Also splits PARTITION/CONCAT/COMPUTE bins into prepare (GPU-origin clone) vs compute when only that kind is active."""
import csv, sys
from collections import defaultdict
d, qn = sys.argv[1], sys.argv[2]
iters = [int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "2,3,4").split(",")]
P = {r["pipeline"]: r["label"] for r in csv.DictReader(open(f"{d}/pipelines.csv"))}
Q = {r["query"]: r for r in csv.DictReader(open(f"{d}/queries.csv"))}
T = list(csv.DictReader(open(f"{d}/tasks.csv")))
PRI = ["SCAN", "COMPUTE", "MERGE", "PARTITION", "CONCAT"]
def cat(label, origin):
    if "GPU_SCAN" in label and origin == "HOST": return "SCAN"
    first = label.split(" ->")[0]
    if first.startswith("PARTITION"): return "PARTITION"
    if first.startswith("CONCAT"): return "CONCAT"
    if first.startswith("MERGE"): return "MERGE"
    return "COMPUTE"
res = defaultdict(float)
for it in iters:
    lab = f"tpch_q{qn}_iter{it}"; t0 = int(Q[lab]["init"]); wall = float(Q[lab]["wall_ms"])
    nb = int(wall * 10) + 1
    act = [dict() for _ in range(nb)]
    for r in T:
        if r["query"] != lab or not r["preparing"] or not r["finalize"]: continue
        c = cat(P[r["pipeline"]], r["origin"])
        pr = int(r["preparing"]); fc = int(r["first_compute"] or r["finalize"]); fi = int(r["finalize"])
        for (a, b, kind) in ((pr, fc, "prep"), (fc, fi, "comp")):
            i0 = max(0, int((a - t0) / 1e5)); i1 = min(nb, int((b - t0) / 1e5) + 1)
            for i in range(i0, i1):
                s = act[i].setdefault(c, set()); s.add(kind)
    for i in range(nb):
        if not act[i]: res["IDLE"] += 0.1 / len(iters); continue
        c = min(act[i], key=PRI.index)
        k = c if c == "SCAN" else c + ("/comp" if "comp" in act[i][c] else "/prep")
        res[k] += 0.1 / len(iters)
    res["_wall"] += wall / len(iters)
for k in sorted(res): print(f"{k:16} {res[k]:8.1f}")
