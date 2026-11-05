#!/usr/bin/env python3
"""Attribute partial-occupancy loss L = integral (1-k/N) dt (k = #GPUs busy, 1<=k<N) to in-flight pipelines.
usage: occ_attr.py <quent_dir> <N> [iters]
Each partial interval's loss is split equally over the distinct pipelines in flight. Pipelines are classed by
task count c: c==1 'single', 1<c<N 'underN', c>=N and c%N!=0 'modN', c%N==0 'skew'. Also by head operator.
Prints suite totals (mean per hot iteration) by class, by head op, and top pipelines."""
import csv, sys, re
from collections import defaultdict
qd, N = sys.argv[1], int(sys.argv[2])
iters = set((sys.argv[3] if len(sys.argv) > 3 else "2,3,4").split(","))
Q = {r["query"]: r for r in csv.DictReader(open(f"{qd}/queries.csv"))}
lab = {r["pipeline"]: r["label"] for r in csv.DictReader(open(f"{qd}/pipelines.csv"))}
tasks = defaultdict(list); pc = defaultdict(int)
for r in csv.DictReader(open(f"{qd}/tasks.csv")):
    if not r["preparing"] or not r["finalize"]: continue
    tasks[r["query"]].append((int(r["preparing"]), int(r["finalize"]), int(r["gpu"]), r["pipeline"]))
    pc[r["pipeline"]] += 1
def cls(c):
    return "single" if c == 1 else "underN" if c < N else "modN" if c % N else "skew"
bycls = defaultdict(float); byop = defaultdict(float); bypipe = defaultdict(float); nit = len(iters)
for ql, q in Q.items():
    qn, it = ql.split("_iter")
    if it not in iters: continue
    t0, t1 = int(q["executing"]), int(q["finished"])
    ev = []
    for (a, b, g, p) in tasks[ql]:
        a, b = max(a, t0), min(b, t1)
        if b > a: ev += [(a, 1, g, p), (b, -1, g, p)]
    ev.sort(key=lambda e: (e[0], e[1]))
    cnt = defaultdict(int); pin = defaultdict(int); prev = t0
    for (t, d, g, p) in ev:
        dt = t - prev; prev = t
        k = sum(1 for x in cnt.values() if x > 0)
        if 1 <= k < N and dt > 0:
            ps = [x for x in pin if pin[x] > 0]; loss = (1 - k / N) * dt / 1e6 / nit
            for x in ps:
                s = loss / len(ps); c = pc[x]
                bycls[cls(c)] += s
                head = re.sub(r"\(.*", "", lab.get(x, "?").split(" -> ")[0])
                byop[head] += s
                bypipe[(qn, lab.get(x, "?")[:70], c)] += s
        cnt[g] += d; pin[p] += d
print("by class (ms, suite, per hot iter):", {k: round(v, 1) for k, v in sorted(bycls.items(), key=lambda x: -x[1])})
print("by head op:")
for k, v in sorted(byop.items(), key=lambda x: -x[1])[:15]: print(f"  {k:28s} {v:7.1f}")
print("top pipelines (query, label, task count per iter*iters):")
for k, v in sorted(bypipe.items(), key=lambda x: -x[1])[:int(__import__("os").environ.get("TOP","40"))]: print(f"  {v:6.1f}  {k[0]:9s} c={k[2]:4d}  {k[1]}")
