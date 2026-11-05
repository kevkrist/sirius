#!/usr/bin/env python3
"""x2_quent.py <quent_dir> [iters]: per query (mean over hot iters), GPU-origin prepare thread-ms and its wall share
(integral of 1/n_active over each task's preparing->first_compute interval; n_active = tasks in preparing..finalize, all GPUs),
split by consumer first operator of the pipeline label; PARTITION op compute thread-ms + wall share."""
import csv, sys, bisect, re
from collections import defaultdict, Counter
qd = sys.argv[1]; ITS = sys.argv[2].split(",") if len(sys.argv) > 2 else ["2", "3", "4"]
pl = {r["pipeline"]: r["label"] for r in csv.DictReader(open(f"{qd}/pipelines.csv"))}
T = defaultdict(list)
for r in csv.DictReader(open(f"{qd}/tasks.csv")):
    T[r["query"]].append(r)
ops = defaultdict(list)
for r in csv.DictReader(open(f"{qd}/task_ops.csv")): ops[r["query"]].append(r)
out = defaultdict(lambda: defaultdict(Counter))
for qlab, ts in T.items():
    m = re.match(r"tpch_q(\d+)_iter(\d+)", qlab)
    if not m or m.group(2) not in ITS: continue
    q = int(m.group(1))
    iv = [(int(t["preparing"]), int(t["finalize"])) for t in ts if t["preparing"] and t["finalize"]]
    ev = sorted([(s, 1) for s, e in iv] + [(e, -1) for s, e in iv])
    X = []; F = []; n = 0; acc = 0.0; last = ev[0][0]
    for t, d in ev:
        if t > last:
            acc += (t - last) / n if n else 0; last = t
        X.append(t); F.append(acc); n += d
    def Fat(t):
        i = bisect.bisect_right(X, t) - 1
        if i < 0: return 0.0
        if i + 1 < len(X) and X[i + 1] > X[i]: return F[i] + (F[i + 1] - F[i]) * (t - X[i]) / (X[i + 1] - X[i])
        return F[i]
    o = out[q]
    for t in ts:
        if not (t["preparing"] and t["first_compute"]): continue
        s, e = int(t["preparing"]), int(t["first_compute"])
        lab = pl.get(t["pipeline"], "?"); first = lab.split("(")[0].strip()
        k = "HOSTprep" if t["origin"] == "HOST" else ("GPUprep:" + first if t["origin"] == "GPU" else "prep:" + t["origin"])
        o[k]["thr"] += (e - s) / 1e6; o[k]["wall"] += (Fat(e) - Fat(s)) / 1e6; o[k]["n"] += 1
    for r in ops[qlab]:
        lab = pl.get(r.get("pipeline", ""), "")
    # PARTITION compute: ops whose pipeline label starts with PARTITION
    for r in ops[qlab]:
        pass
    o["_iters"]["n"] += 1
    o["_wall"]["ms"] += (max(e for s, e in iv) - min(s for s, e in iv)) / 1e6
# partition compute via task_ops joined to tasks' pipeline
tp = {}
for qlab, ts in T.items():
    for t in ts: tp[t["task"]] = pl.get(t["pipeline"], "")
res = []
for q in sorted(out):
    o = out[q]; k = o["_iters"]["n"]
    row = {"query": q, "wall_ms": round(o["_wall"]["ms"] / k, 1)}
    for key, c in o.items():
        if key.startswith("_"): continue
        row[key + ".thr"] = round(c["thr"] / k, 1); row[key + ".wall"] = round(c["wall"] / k, 1); row[key + ".n"] = round(c["n"] / k)
    res.append(row)
keys = sorted({k for r in res for k in r}, key=lambda x: (x not in ("query", "wall_ms"), x))
w = csv.DictWriter(sys.stdout, keys); w.writeheader(); [w.writerow(r) for r in res]
