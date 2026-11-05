#!/usr/bin/env python3
"""Classify g4 wall excess over g1/4 by mechanism class from phase_*.json (Quent wall attribution)."""
import json, re, sys
from collections import defaultdict
base = sys.argv[1]
TIM = {}
import csv
for run in ("g1_a", "g4_a"):
    for r in csv.DictReader(open(f"/home/ubuntu/sirius-scaling-20261105/00-baseline/{run}/timings.csv")):
        if r["iter"] in ("2", "3", "4"):
            TIM.setdefault((run, r["query"]), []).append(float(r["seconds"]) * 1e3)
def cls(name):
    if name == "scanH2D": return "scan"
    if name == "idle/gap": return "gap"
    op = name.split(":")[1]
    if op.startswith("CONCAT") or op.startswith("MERGE_GROUP_BY"): return "xgpu_gather"
    if op.startswith("PARTITION"): return "partition"
    if op.startswith(("MERGE_TOP_N", "SORT_SAMPLE", "MERGE_SORT", "RESULT", "MERGE_AGGREGATE", "PROJECTION", "NESTED_LOOP")): return "serial_tail"
    if op.startswith("GPU_SCAN") or op.startswith("COLUMN_DATA_SCAN"): return "scan_compute_tail"
    return "compute"
tot = defaultdict(float)
for q in (2, 11, 16, 22):
    a = json.load(open(f"{base}/phase_g1_a_q{q}.json")); b = json.load(open(f"{base}/phase_g4_a_q{q}.json"))
    g1 = sum(TIM[("g1_a", str(q))]) / 3; g4 = sum(TIM[("g4_a", str(q))]) / 3
    ex = defaultdict(float); per = defaultdict(list)
    for k in set(a["acc"]) | set(b["acc"]):
        e = b["acc"].get(k, 0) - a["acc"].get(k, 0) / 4
        ex[cls(k)] += e; per[cls(k)].append((e, k))
    ex["outside_engine"] = b["outside"] - a["outside"] / 4
    print(f"Q{q}: g1 {g1:.1f} g4 {g4:.1f} ideal {g1/4:.1f} gap {g4-g1/4:.1f} | quent g1 {a['wall']:.1f} g4 {b['wall']:.1f} outside g1 {a['outside']:.1f} g4 {b['outside']:.1f}")
    for c, v in sorted(ex.items(), key=lambda kv: -kv[1]):
        top = ", ".join(f"{k}={e:+.1f}" for e, k in sorted(per[c], reverse=True)[:4]) if c in per else ""
        print(f"   {c:18s} {v:+7.1f}  {top}")
        tot[c] += v
    print(f"   sum {sum(ex.values()):.1f}  (unexplained vs timings gap: {g4-g1/4-sum(ex.values()):+.1f})")
    tot["gap_total"] += g4 - g1 / 4
print("FAMILY:", {k: round(v, 1) for k, v in tot.items()})
