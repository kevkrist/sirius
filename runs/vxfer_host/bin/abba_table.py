#!/usr/bin/env python3
"""abba_table.py <label>[,<label>...] [variant letters, default AH]: per-query medians per arm family (first letter of the
arm dir), delta vs A. Hot = iterations >= 2 (iter0 warm, iter1 = first hot, dropped). result.txt md5 compared across all runs."""
import csv, glob, statistics as st, sys, re, os, hashlib
X = "/opt/dlami/nvme/dfperf/runs/vxfer_host"
labels = sys.argv[1].split(","); fams = list(sys.argv[2]) if len(sys.argv) > 2 else ["A", "H"]
def runs(label, fam, q):
    out = []
    for d in sorted(glob.glob(f"{X}/{label}/{fam}*/tpch_*_q{q}")):
        try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception: continue
        t = {int(r["iteration"]): float(r["runtime_s"]) for r in rows if r.get("query") in (f"q{q}", str(q))}
        res = f"{d}/sirius/q{q}/result.txt"
        h = hashlib.md5(open(res, "rb").read()).hexdigest()[:8] if os.path.exists(res) else "missing"
        hot = [t[i] * 1000 for i in sorted(t) if i >= 2]
        if hot: out.append((hot, h, os.path.basename(os.path.dirname(d))))
    return out
qs = sorted({int(re.search(r"_q(\d+)$", d).group(1)) for l in labels for d in glob.glob(f"{X}/{l}/A*/tpch_*_q*")})
hdr = "| q | A med ms (n) [min-max] |" + "".join(f" {v} med (n) [min-max] | {v}-A ms (%) | per-process medians A / {v} |" for v in fams[1:]) + " results |"
print(hdr); print("|" + "---|" * (hdr.count("|") - 1))
tot = {v: 0.0 for v in fams}; full = {v: True for v in fams}
for q in qs:
    data = {v: [r for l in labels for r in runs(l, v, q)] for v in fams}
    hs = {h for v in fams for _, h, _ in data[v]}
    a = [x for r, _, _ in data["A"] for x in r]
    if not a: continue
    ma = st.median(a); tot["A"] += ma
    row = f"| {q} | {ma:.1f} ({len(a)}) [{min(a):.0f}-{max(a):.0f}] |"
    for v in fams[1:]:
        xs = [x for r, _, _ in data[v] for x in r]
        if not xs: row += " - | - | - |"; full[v] = False; continue
        m = st.median(xs); tot[v] += m
        pa = " ".join(f"{st.median(r):.0f}" for r, _, _ in data["A"]); pv = " ".join(f"{st.median(r):.0f}" for r, _, _ in data[v])
        row += f" {m:.1f} ({len(xs)}) [{min(xs):.0f}-{max(xs):.0f}] | {m-ma:+.1f} ({100*(m-ma)/ma:+.2f}%) | {pa} / {pv} |"
    row += " identical |" if len(hs) == 1 else f" DIFF {sorted(hs)} |"
    print(row)
print("| sum | %.0f |" % tot["A"] + "".join((f" {tot[v]:.0f} | {tot[v]-tot['A']:+.1f} ({100*(tot[v]-tot['A'])/tot['A']:+.2f}%) | |" if full[v] else " (partial) | | |") for v in fams[1:]) + " |")
