#!/usr/bin/env python3
"""abba_arms.py <tag>: per query the median of each of the 4 processes (tipA, protoA, protoB, tipB), hot iterations 2..5."""
import csv, glob, statistics as st, sys, re
B = "/opt/dlami/nvme/dfperf/runs/xfer_d2d"; tag = sys.argv[1]
def med(label, q):
    d = sorted(glob.glob(f"{B}/{tag}_{label}/tpch_*_q{q}"))[-1]
    t = [float(r["runtime_s"]) * 1000 for r in csv.DictReader(open(f"{d}/csv/runtimes.csv")) if int(r["iteration"]) >= 2]
    return st.median(t), len(t)
qs = sorted({int(re.search(r"_q(\d+)$", d).group(1)) for d in glob.glob(f"{B}/{tag}_tipA/tpch_*_q*")})
print("| q | tipA | protoA | protoB | tipB | tipA-tipB | protoA-protoB | proto-tip (mean of process medians) |")
print("|---|---|---|---|---|---|---|---|")
tot = [0, 0, 0, 0]
for q in qs:
    v = [med(l, q)[0] for l in ("tipA", "protoA", "protoB", "tipB")]
    tot = [a + b for a, b in zip(tot, v)]
    print(f"| {q} | {v[0]:.1f} | {v[1]:.1f} | {v[2]:.1f} | {v[3]:.1f} | {v[0]-v[3]:+.1f} | {v[1]-v[2]:+.1f} | {(v[1]+v[2]-v[0]-v[3])/2:+.1f} |")
print(f"| sum | {tot[0]:.0f} | {tot[1]:.0f} | {tot[2]:.0f} | {tot[3]:.0f} | {tot[0]-tot[3]:+.0f} | {tot[1]-tot[2]:+.0f} | {(tot[1]+tot[2]-tot[0]-tot[3])/2:+.0f} |")
