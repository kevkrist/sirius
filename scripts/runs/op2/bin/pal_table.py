#!/usr/bin/env python3
"""pal_table.py <tag>[,<tag>...] <variant> [<variant>...]: per-query medians vs 'tip' from runs/op2/<tag>_<variant>_{A,B}.
Hot = iterations >= 2 (iter0 warm, iter1 first hot dropped). Pools A+B and all tags. result.txt md5 compared across all runs."""
import csv, glob, statistics as st, sys, re, os, hashlib
B = "/opt/dlami/nvme/dfperf/runs/op2"
tags = sys.argv[1].split(","); variants = ["tip"] + [v for v in sys.argv[2:] if v != "tip"]
def runs(label, q):
    out = []
    for d in sorted(glob.glob(f"{B}/{label}/tpch_*_q{q}")):
        try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception: continue
        t = {int(r["iteration"]): float(r["runtime_s"]) for r in rows if r.get("query") in (f"q{q}", str(q))}
        res = f"{d}/sirius/q{q}/result.txt"
        h = hashlib.md5(open(res, "rb").read()).hexdigest()[:8] if os.path.exists(res) else "missing"
        hot = [t[i] * 1000 for i in sorted(t) if i >= 2]
        if hot: out.append((hot, h))
    return out
qs = sorted({int(re.search(r"_q(\d+)$", d).group(1)) for t in tags for d in glob.glob(f"{B}/{t}_tip_A/tpch_*_q*")})
hdr = "| q | tip med ms (n) [min-max] |" + "".join(f" {v} med (n) [min-max] | {v} delta ms (%) | per-pass deltas |" for v in variants[1:]) + " results |"
print(hdr); print("|" + "---|" * (hdr.count("|") - 1))
tot = {v: 0.0 for v in variants}; full = {v: True for v in variants}
for q in qs:
    data = {v: [r for t in tags for p in "AB" for r in runs(f"{t}_{v}_{p}", q)] for v in variants}
    per = {v: {(t, p): [x for r in runs(f"{t}_{v}_{p}", q) for x in r[0]] for t in tags for p in "AB"} for v in variants}
    hs = {h for v in variants for _, h in data[v]}
    tip = [x for r, _ in data["tip"] for x in r]
    if not tip: continue
    mt = st.median(tip); tot["tip"] += mt
    row = f"| {q} | {mt:.1f} ({len(tip)}) [{min(tip):.0f}-{max(tip):.0f}] |"
    for v in variants[1:]:
        xs = [x for r, _ in data[v] for x in r]
        if not xs: row += " - | - | - |"; full[v] = False; continue
        m = st.median(xs); tot[v] += m
        pd = " ".join(f"{st.median(per[v][k]) - st.median(per['tip'][k]):+.1f}" for k in per[v] if per[v][k] and per["tip"][k])
        row += f" {m:.1f} ({len(xs)}) [{min(xs):.0f}-{max(xs):.0f}] | {m-mt:+.1f} ({100*(m-mt)/mt:+.2f}%) | {pd} |"
    row += " identical |" if len(hs) == 1 else f" DIFF {sorted(hs)} |"
    print(row)
print("| sum | %.0f |" % tot["tip"] + "".join((f" {tot[v]:.0f} | {tot[v]-tot['tip']:+.1f} ({100*(tot[v]-tot['tip'])/tot['tip']:+.2f}%) | |" if full[v] else " (partial) | | |") for v in variants[1:]) + " |")
