#!/usr/bin/env python3
"""Per-pipeline time accounting for one query from Quent flattened tables.
usage: pipe_acct.py <quent_dir> <query_label_prefix e.g. tpch_q3_iter> [iters=2,3,4]
Thread-time per pipeline (ms, mean over iters): tasks, host-origin prepare, gpu-origin prepare,
compute (first_compute->finalize), span (first preparing -> last finalize, offset from query init),
bytes_in, and per GPU busy.
"""
import csv, sys
from collections import defaultdict
qd, qp = sys.argv[1], sys.argv[2]
iters = [int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "2,3,4").split(",")]
labels = {f"{qp}{i}" for i in iters}
qinit = {}
for r in csv.DictReader(open(f"{qd}/queries.csv")):
    if r["query"] in labels:
        qinit[r["query"]] = (int(r["init"]), float(r["wall_ms"]))
plab = {}
for r in csv.DictReader(open(f"{qd}/pipelines.csv")):
    if r["query"] in labels:
        plab[r["pipeline"]] = (int(r["type_name"].split()[-1]), r["label"])
agg = defaultdict(lambda: defaultdict(float))
spans = defaultdict(lambda: [1e30, 0])
n = len(labels)
for r in csv.DictReader(open(f"{qd}/tasks.csv")):
    if r["query"] not in labels or r["pipeline"] not in plab:
        continue
    pid = plab[r["pipeline"]][0]
    t0 = qinit[r["query"]][0]
    try:
        prep, fc, fin = int(r["preparing"]), int(r["first_compute"]), int(r["finalize"])
    except ValueError:
        continue
    a = agg[pid]
    a["tasks"] += 1 / n
    a["prep_" + r["origin"]] += (fc - prep) / 1e6 / n
    a["comp"] += (fin - fc) / 1e6 / n
    a["bytes_" + r["origin"]] += int(r["in_bytes"] or 0) / n
    a["gpu%s" % r["gpu"]] += (fin - prep) / 1e6 / n
    s = spans[(pid, r["query"])]
    s[0] = min(s[0], (prep - t0) / 1e6); s[1] = max(s[1], (fin - t0) / 1e6)
wall = sum(v[1] for v in qinit.values()) / n
print(f"# {qd} {qp}{iters} mean wall {wall:.1f} ms")
print(f"{'pid':>3} {'label':58} {'tasks':>6} {'prepH':>8} {'prepG':>8} {'comp':>8} {'start':>7} {'end':>7} {'GB_H':>6} {'GB_G':>6}")
tot = defaultdict(float)
for pid in sorted(agg):
    a = agg[pid]
    st = sum(spans[(pid, q)][0] for q in labels if (pid, q) in spans) / n
    en = sum(spans[(pid, q)][1] for q in labels if (pid, q) in spans) / n
    lab = [l for p, l in plab.values() if p == pid][0]
    print(f"{pid:>3} {lab[:58]:58} {a['tasks']:6.0f} {a['prep_HOST']:8.1f} {a['prep_GPU']:8.1f} {a['comp']:8.1f} {st:7.1f} {en:7.1f} {a['bytes_HOST']/1e9:6.1f} {a['bytes_GPU']/1e9:6.1f}")
    for k in ("prep_HOST", "prep_GPU", "comp", "tasks"):
        tot[k] += a[k]
print(f"TOT {'':58} {tot['tasks']:6.0f} {tot['prep_HOST']:8.1f} {tot['prep_GPU']:8.1f} {tot['comp']:8.1f}  thread-total {tot['prep_HOST']+tot['prep_GPU']+tot['comp']:.1f}")
