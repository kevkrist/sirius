#!/usr/bin/env python3
"""x2_pipes.py <quent_dir> <query_label>: per pipeline tasks, origin, input GB, prepare thr ms, compute thr ms, span."""
import csv, sys
from collections import defaultdict, Counter
qd, ql = sys.argv[1], sys.argv[2]
pl = {r["pipeline"]: (r["type_name"], r["label"]) for r in csv.DictReader(open(f"{qd}/pipelines.csv")) if r["query"] == ql}
A = defaultdict(Counter); span = {}
q0 = None
rows = [r for r in csv.DictReader(open(f"{qd}/tasks.csv")) if r["query"] == ql]
q0 = min(int(r["preparing"]) for r in rows if r["preparing"])
for r in rows:
    a = A[r["pipeline"]]; a["n"] += 1; a["in"] += int(r["in_bytes"] or 0); a["origin_" + r["origin"]] += 1
    if r["preparing"] and r["first_compute"] and r["finalize"]:
        p, f, z = int(r["preparing"]), int(r["first_compute"]), int(r["finalize"])
        a["prep"] += (f - p) / 1e6; a["comp"] += (z - f) / 1e6
        s = span.get(r["pipeline"], (1 << 62, 0)); span[r["pipeline"]] = (min(s[0], p), max(s[1], z))
for pid, a in sorted(A.items(), key=lambda x: span.get(x[0], (0, 0))[0]):
    s = span.get(pid, (q0, q0))
    t, lab = pl.get(pid, ("?", "?"))
    org = ",".join(f"{k[7:]}:{v}" for k, v in a.items() if k.startswith("origin_"))
    print(f"{t:16s} {lab[:70]:70s} n={a['n']:5d} {org:18s} in={a['in']/1e9:7.2f}GB prep={a['prep']:8.1f} comp={a['comp']:8.1f} span=[{(s[0]-q0)/1e6:6.0f},{(s[1]-q0)/1e6:6.0f}]")
