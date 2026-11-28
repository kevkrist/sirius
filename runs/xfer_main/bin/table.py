#!/usr/bin/env python3
"""table.py <tag> [<tag> ...]: per query: median of pooled hot iterations (iteration >= 2) per arm, delta, md5 agreement."""
import csv, glob, statistics as st, sys, re, hashlib, os
B = "/opt/dlami/nvme/dfperf/runs/xfer_main"; A, Bn = sys.argv[1], sys.argv[2]; tags = sys.argv[3:]
def procs(arm, q):
    out = []
    for tag in tags:
        for half in "AB":
            for d in sorted(glob.glob(f"{B}/{tag}_{arm}{half}/tpch_*_q{q}"))[-1:]:
                f = f"{d}/csv/runtimes.csv"
                if not os.path.exists(f): continue
                t = [float(r["runtime_s"]) * 1000 for r in csv.DictReader(open(f)) if int(r["iteration"]) >= 2 and r["query"] != "TOTAL"]
                out.append((t, d))
    return out
def md5s(arm, q):
    s = set()
    for tag in tags:
        for f in glob.glob(f"{B}/{tag}_{arm}?/tpch_*_q{q}/sirius/q{q}/result.txt"):
            data = open(f, "rb").read()
            if q == 11: data = b"\n".join(sorted(data.split(b"\n")))
            s.add(hashlib.md5(data).hexdigest())
    return s
qs = sorted({int(re.search(r"_q(\d+)$", d).group(1)) for tag in tags for d in glob.glob(f"{B}/{tag}_{Bn}?/tpch_*_q*")})
print("| q | n A/B | A | B | B-A | process medians A / B | spread A / B | result |")
print("|---|---|---|---|---|---|---|---|")
tt = tp = 0.0
for q in qs:
    m = {}; pm = {}; sp = {}; n = {}
    for a in (A, Bn):
        p = procs(a, q); pooled = [x for t, _ in p for x in t]; n[a] = len(pooled)
        if not pooled: break
        m[a] = st.median(pooled); pm[a] = " ".join(f"{st.median(t):.0f}" for t, _ in p); sp[a] = max(pooled) - min(pooled)
    if len(m) < 2: continue
    tt += m[A]; tp += m[Bn]
    same = md5s(A, q) | md5s(Bn, q)
    print(f"| {q} | {n[A]}/{n[Bn]} | {m[A]:.1f} | {m[Bn]:.1f} | {m[Bn]-m[A]:+.1f} | {pm[A]} / {pm[Bn]} | {sp[A]:.0f} / {sp[Bn]:.0f} | {'same' if len(same) == 1 else 'DIFF %d' % len(same)} |")
print(f"| sum | | {tt:.0f} | {tp:.0f} | {tp-tt:+.0f} | | | |")
