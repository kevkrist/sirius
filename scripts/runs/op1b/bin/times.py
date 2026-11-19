#!/usr/bin/env python3
"""times.py <tagA-labels,comma> <tagB-labels,comma> [q ...]: per-query medians, dropping iter0 (warm) and iter1 (first hot)."""
import csv, glob, statistics as st, sys, re
B = "/opt/dlami/nvme/dfperf/runs/op1b"
def hot(label, q):
    out = []
    for d in sorted(glob.glob(f"{B}/{label}/tpch_*_q{q}")):
        try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception: continue
        t = {}
        for r in rows:
            if r.get("query") in (f"q{q}", str(q)):
                try: t[int(r["iteration"])] = float(r["runtime_s"]) * 1000
                except Exception: pass
        out.append([t[i] for i in sorted(t) if i >= 2])
    return out
A = sys.argv[1].split(","); Bl = sys.argv[2].split(",")
qs = [int(x) for x in sys.argv[3:]] or list(range(1, 23))
print("| q | A med ms (n, min..max) | per-process A | B med ms (n, min..max) | per-process B | B-A ms | B/A |")
print("|---|---|---|---|---|---|---|")
ta = tb = 0.0; ok = True
for q in qs:
    pa = [h for l in A for h in hot(l, q)]; pb = [h for l in Bl for h in hot(l, q)]
    a = [x for h in pa for x in h]; b = [x for h in pb for x in h]
    if not a or not b: print(f"| {q} | incomplete |"); ok = False; continue
    ma, mb = st.median(a), st.median(b); ta += ma; tb += mb
    f = lambda v: f"{st.median(v):.1f} ({len(v)}, {min(v):.0f}..{max(v):.0f})"
    pp = lambda ps: "/".join(f"{st.median(h):.0f}" for h in ps if h)
    print(f"| {q} | {f(a)} | {pp(pa)} | {f(b)} | {pp(pb)} | {mb-ma:+.1f} | {mb/ma:.4f} |")
print(f"| sum | {ta:.0f} | | {tb:.0f} | | {tb-ta:+.0f} | {tb/ta if ta else 0:.4f} |")
