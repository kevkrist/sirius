#!/usr/bin/env python3
"""Wall-time phase attribution of one query (Quent flattened dir), averaged over iters.
usage: phase.py <quent_dir> <query> [iters=2,3,4] [timings.csv]
1 ms bins over [executing, finished]. Each bin is attributed:
  'scanH2D'  : >=1 HOST-origin task in preparing (upload/decode) on any GPU
  else split evenly among pipelines with an active task (preparing..finalize), else 'idle/gap'.
Also prints, per GPU, busy fraction; and outside-engine time (timings - quent wall) if timings given."""
import csv, sys
from collections import defaultdict
D, Q = sys.argv[1], sys.argv[2]
iters = [int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "2,3,4").split(",")]
tim = sys.argv[4] if len(sys.argv) > 4 else None
qs = {r["query"]: r for r in csv.DictReader(open(f"{D}/queries.csv"))}
pl = {r["pipeline"]: r["label"] for r in csv.DictReader(open(f"{D}/pipelines.csv"))}
T = defaultdict(list)
for r in csv.DictReader(open(f"{D}/tasks.csv")):
    if r["query"].startswith(f"tpch_q{Q}_iter"):
        T[r["query"]].append(r)
acc = defaultdict(float); wall = 0; busy = defaultdict(float); gpus = set()
outside = []
if tim:
    tt = {(r["query"], r["iter"]): float(r["seconds"]) for r in csv.DictReader(open(tim))}
for it in iters:
    lab = f"tpch_q{Q}_iter{it}"; q = qs[lab]
    t0 = int(q["executing"]); t1 = int(q["finished"]); n = (t1 - t0) // 1_000_000 + 1
    wall += (t1 - t0) / 1e6 / len(iters)
    if tim: outside.append(tt[(Q, str(it))] * 1e3 - float(q["wall_ms"]))
    h2d = [False] * n; act = [set() for _ in range(n)]
    gb = defaultdict(lambda: [False] * n)
    for r in T[lab]:
        if not (r["preparing"] and r["finalize"]): continue
        p = (int(r["preparing"]) - t0) // 1_000_000; f = (int(r["finalize"]) - t0) // 1_000_000
        fc = (int(r["first_compute"]) - t0) // 1_000_000 if r["first_compute"] else f
        name = r["pipeline_type"].split()[-1] + ":" + pl.get(r["pipeline"], "?").split(" ->")[0]
        gpus.add(r["gpu"])
        for i in range(max(p, 0), min(f + 1, n)):
            act[i].add(name); gb[r["gpu"]][i] = True
        if r["origin"] == "HOST":
            for i in range(max(p, 0), min(fc + 1, n)):
                h2d[i] = True
    for i in range(n):
        if h2d[i]: acc["scanH2D"] += 1 / len(iters)
        elif act[i]:
            for a in act[i]: acc[a] += 1 / len(act[i]) / len(iters)
        else: acc["idle/gap"] += 1 / len(iters)
    for g, v in gb.items(): busy[g] += sum(v) / n / len(iters)
import json,os
if os.environ.get("JSON"): json.dump(dict(acc=acc,wall=wall,outside=(sum(outside)/len(outside) if outside else 0),busy=busy),open(os.environ["JSON"],"w"))
print(f"Q{Q} {D}: quent wall {wall:.1f} ms" + (f"; outside-engine (timings-quent) mean {sum(outside)/len(outside):.1f} ms" if outside else ""))
print("  per-GPU busy frac: " + " ".join(f"g{g}={busy[g]:.2f}" for g in sorted(busy)))
for k, v in sorted(acc.items(), key=lambda kv: -kv[1]):
    if v >= 0.5: print(f"  {k:40s} {v:6.1f} ms")
