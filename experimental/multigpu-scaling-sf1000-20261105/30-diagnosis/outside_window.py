#!/usr/bin/env python3
"""Per-query time outside the Quent init->finished window (timings - quent wall), hot iters 2-4."""
import csv, sys
def load(run):
    t = {}
    for r in csv.DictReader(open(f"{run}/timings.csv")):
        if r["query"].isdigit() and int(r["iter"]) >= 2:
            t[(int(r["query"]), int(r["iter"]))] = float(r["seconds"]) * 1000
    q = {}
    for r in csv.DictReader(open(f"{run}/quent/queries.csv")):
        lab = r["query"]  # tpch_q1_iter2
        qn = int(lab.split("_")[1][1:]); it = int(lab.split("iter")[1])
        if it >= 2: q[(qn, it)] = float(r["wall_ms"])
    out = {}
    for k in t:
        if k in q: out.setdefault(k[0], []).append(t[k] - q[k])
    return {k: sum(v) / len(v) for k, v in out.items()}
g1 = load("00-baseline/g1_a"); g4 = load("00-baseline/g4_a")
tot1 = tot4 = gap = 0
for qn in sorted(g1):
    d = g4[qn] - g1[qn] / 4; tot1 += g1[qn]; tot4 += g4[qn]; gap += d
    print(f"Q{qn:<3} out_g1 {g1[qn]:7.1f} out_g4 {g4[qn]:7.1f} gap_share {d:7.1f}")
print(f"TOTAL out_g1 {tot1:.0f} ms, out_g4 {tot4:.0f} ms, gap share (g4 - g1/4) {gap:.0f} ms")
