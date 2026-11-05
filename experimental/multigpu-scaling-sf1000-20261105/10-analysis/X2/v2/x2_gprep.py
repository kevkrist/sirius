#!/usr/bin/env python3
"""x2_gprep.py <quent_dir>...: per query (hot iters 2-4 mean) GPU-origin prepare (cross-GPU clone) thread ms, wall share ms,
GPU-origin input GB, task count. CSV to stdout, one block per run."""
import csv, sys, bisect, re
from collections import defaultdict, Counter
print("run,query,gprep_n,gprep_GB,gprep_thr_ms,gprep_share_ms")
for qd in sys.argv[1:]:
    T = defaultdict(list)
    for r in csv.DictReader(open(f"{qd}/tasks.csv")): T[r["query"]].append(r)
    acc_q = defaultdict(Counter)
    for ql, ts in T.items():
        m = re.match(r"tpch_q(\d+)_iter(\d+)", ql)
        if not m or m.group(2) not in ("2", "3", "4"): continue
        q = int(m.group(1))
        iv = [(int(t["preparing"]), int(t["finalize"])) for t in ts if t["preparing"] and t["finalize"]]
        ev = sorted([(s, 1) for s, e in iv] + [(e, -1) for s, e in iv]); X = []; F = []; n = 0; acc = 0.0; last = ev[0][0]
        for t, d in ev:
            if t > last: acc += (t - last) / n if n else 0; last = t
            X.append(t); F.append(acc); n += d
        def Fat(t):
            i = bisect.bisect_right(X, t) - 1
            if i < 0: return 0.0
            if i + 1 < len(X) and X[i + 1] > X[i]: return F[i] + (F[i + 1] - F[i]) * (t - X[i]) / (X[i + 1] - X[i])
            return F[i]
        a = acc_q[q]; a["k"] += 1
        for t in ts:
            if t["origin"] != "GPU" or not (t["preparing"] and t["first_compute"]): continue
            p, f = int(t["preparing"]), int(t["first_compute"])
            a["n"] += 1; a["GB"] += int(t["in_bytes"] or 0) / 1e9; a["thr"] += (f - p) / 1e6; a["sh"] += (Fat(f) - Fat(p)) / 1e6
    for q in sorted(acc_q):
        a = acc_q[q]; k = a["k"]
        print(f"{qd.rstrip('/').split('/')[-1] if 'quent' not in qd else qd.split('/')[-3]},{q},{a['n']/k:.0f},{a['GB']/k:.2f},{a['thr']/k:.0f},{a['sh']/k:.0f}")
