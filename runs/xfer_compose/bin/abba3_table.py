#!/usr/bin/env python3
"""abba3_table.py <tag> [<tag> ...]: per query, per arm: median of the pooled hot iterations (>= 2) of all processes,
process medians, deltas vs tip, result md5 agreement."""
import csv, glob, statistics as st, sys, re, hashlib, os
B = "/opt/dlami/nvme/dfperf/runs/xfer_compose"; tags = sys.argv[1:]
arms = ["tip", "cpu", "gpu"]
def procs(arm, q):
    out = []
    for tag in tags:
        for half in "AB":
            for d in sorted(glob.glob(f"{B}/{tag}_{arm}{half}/tpch_*_q{q}"))[-1:]:
                t = [float(r["runtime_s"]) * 1000 for r in csv.DictReader(open(f"{d}/csv/runtimes.csv")) if int(r["iteration"]) >= 2 and r["query"] != "TOTAL"]
                out.append((t, d))
    return out
qs = sorted({int(re.search(r"_q(\d+)$", d).group(1)) for tag in tags for d in glob.glob(f"{B}/{tag}_tipB/tpch_*_q*") if os.path.exists(d + "/csv/runtimes.csv")})
print("| q | n/arm | tip | cpu-OR (compose) | gpu-OR (pipe) | cpu-tip | gpu-tip | cpu-gpu | process medians tip / cpu / gpu | spread tip / cpu / gpu |")
print("|---|---|---|---|---|---|---|---|---|---|")
tot = {a: 0.0 for a in arms}
for q in qs:
    m = {}; pm = {}; sp = {}; n = 0
    for a in arms:
        p = procs(a, q); pooled = [x for t, _ in p for x in t]; n = len(pooled)
        m[a] = st.median(pooled); pm[a] = " ".join(f"{st.median(t):.0f}" for t, _ in p); sp[a] = max(pooled) - min(pooled)
        tot[a] += m[a]
    print(f"| {q} | {n} | {m['tip']:.1f} | {m['cpu']:.1f} | {m['gpu']:.1f} | {m['cpu']-m['tip']:+.1f} | {m['gpu']-m['tip']:+.1f} | {m['cpu']-m['gpu']:+.1f} | {pm['tip']} / {pm['cpu']} / {pm['gpu']} | {sp['tip']:.0f} / {sp['cpu']:.0f} / {sp['gpu']:.0f} |")
print(f"| sum | | {tot['tip']:.0f} | {tot['cpu']:.0f} | {tot['gpu']:.0f} | {tot['cpu']-tot['tip']:+.0f} | {tot['gpu']-tot['tip']:+.0f} | {tot['cpu']-tot['gpu']:+.0f} | | |")
