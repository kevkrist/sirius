#!/usr/bin/env python3
"""pipe_wall.py <quent_dir> <qnum> [iters]: exclusive wall per pipeline (0.1ms bins; a bin goes to the in-flight pipeline with
lowest pipeline id among SCAN pipelines first, else the lowest pipeline id), mean over iters. Prints pid, label, wall_ms, prep-only share."""
import csv, sys
from collections import defaultdict
d, qn = sys.argv[1], sys.argv[2]
iters = [int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "2,3,4").split(",")]
P = {r["pipeline"]: (int(r["type_name"].split()[-1]), r["label"]) for r in csv.DictReader(open(f"{d}/pipelines.csv"))}
Q = {r["query"]: r for r in csv.DictReader(open(f"{d}/queries.csv"))}
T = list(csv.DictReader(open(f"{d}/tasks.csv")))
res = defaultdict(float); prep = defaultdict(float); lab = {}
for it in iters:
    L = f"tpch_q{qn}_iter{it}"; t0 = int(Q[L]["init"]); wall = float(Q[L]["wall_ms"]); nb = int(wall * 10) + 1
    act = [dict() for _ in range(nb)]
    for r in T:
        if r["query"] != L or not r["preparing"] or not r["finalize"]: continue
        pid, label = P[r["pipeline"]]; lab[pid] = label
        scan = "GPU_SCAN" in label and r["origin"] == "HOST"
        pr = int(r["preparing"]); fc = int(r["first_compute"] or r["finalize"]); fi = int(r["finalize"])
        for (a, b, k) in ((pr, fc, "p"), (fc, fi, "c")):
            for i in range(max(0, int((a - t0) / 1e5)), min(nb, int((b - t0) / 1e5) + 1)):
                act[i].setdefault((0 if scan else 1, pid), set()).add(k)
    for i in range(nb):
        if not act[i]: res[-1] += 0.1 / len(iters); continue
        key = min(act[i]); res[key[1]] += 0.1 / len(iters)
        if act[i][key] == {"p"}: prep[key[1]] += 0.1 / len(iters)
    res[-2] += wall / len(iters)
lab[-1] = "IDLE"; lab[-2] = "WALL"
for k in sorted(res): print(f"{k},{lab[k]},{res[k]:.1f},{prep[k]:.1f}")
