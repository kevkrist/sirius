#!/usr/bin/env python3
"""Occupancy bound for granularity/imbalance at N GPUs.
usage: occupancy.py <quent_dir> <N> [iters=2,3,4]
For each query (hot iters), window [executing, finished]. GPU busy = union of [preparing, finalize] of its tasks.
Outputs per query (mean over iters, ms): wall, Z (no GPU busy), P (partial: 1..N-1 GPUs busy),
L = integral over partial periods of (1 - k/N) dt  -> wall saved if the work in those periods were spread
perfectly over N GPUs (optimistic upper bound for any imbalance/granularity fix),
L1 = same but only periods where exactly 1 GPU busy AND every in-flight task belongs to a 1-task pipeline (serial chain: not
fixable by repartitioning), Lm = L - L1.
"""
import csv, sys
from collections import defaultdict
qd, N = sys.argv[1], int(sys.argv[2])
iters = set((sys.argv[3] if len(sys.argv) > 3 else "2,3,4").split(","))
Q = {r["query"]: r for r in csv.DictReader(open(f"{qd}/queries.csv"))}
tasks = defaultdict(list); pipecount = defaultdict(int)
for r in csv.DictReader(open(f"{qd}/tasks.csv")):
    if not r["preparing"] or not r["finalize"]: continue
    tasks[r["query"]].append((int(r["preparing"]), int(r["finalize"]), int(r["gpu"]), r["pipeline"]))
    pipecount[(r["query"], r["pipeline"])] += 1
res = defaultdict(lambda: defaultdict(list))
for lab, q in Q.items():
    qn, it = lab.split("_iter")
    if it not in iters: continue
    t0, t1 = int(q["executing"]), int(q["finished"])
    ev = []
    for (a, b, g, p) in tasks[lab]:
        a, b = max(a, t0), min(b, t1)
        if b <= a: continue
        ev.append((a, 1, g, p)); ev.append((b, -1, g, p))
    ev.sort(key=lambda e: (e[0], e[1]))
    cnt = defaultdict(int); pin = defaultdict(int)
    Z = P = L = L1 = 0.0; prev = t0
    def acc(dt):
        global Z, P, L, L1
        k = sum(1 for g in cnt if cnt[g] > 0)
        if k == 0: Z += dt
        elif k < N:
            P += dt; L += (1 - k / N) * dt
            if k == 1 and all(pipecount[(lab, p)] == 1 for p in pin if pin[p] > 0): L1 += (1 - 1 / N) * dt
    for (t, d, g, p) in ev:
        acc(t - prev); prev = t
        cnt[g] += d; pin[p] += d
    acc(t1 - prev)
    for k, v in (("wall", t1 - t0), ("Z", Z), ("P", P), ("L", L), ("L1", L1)):
        res[qn][k].append(v / 1e6)
tot = defaultdict(float)
print(f"{'q':10s} {'wall':>8s} {'Z':>7s} {'P':>7s} {'L':>7s} {'L1':>7s} {'Lm':>7s}")
for qn in sorted(res, key=lambda s: int(s.split('q')[-1])):
    m = {k: sum(v) / len(v) for k, v in res[qn].items()}
    m["Lm"] = m["L"] - m["L1"]
    for k in m: tot[k] += m[k]
    print(f"{qn:10s} {m['wall']:8.1f} {m['Z']:7.1f} {m['P']:7.1f} {m['L']:7.1f} {m['L1']:7.1f} {m['Lm']:7.1f}")
print(f"{'TOTAL':10s} {tot['wall']:8.1f} {tot['Z']:7.1f} {tot['P']:7.1f} {tot['L']:7.1f} {tot['L1']:7.1f} {tot['Lm']:7.1f}")
