#!/usr/bin/env python3
"""table.py <label> [<label> ...] : ABBA table. Arms A* = tip, B* = proto. Drops iteration 0 (warm) and 1 (first hot)."""
import csv, glob, re, statistics as st, sys, os, filecmp
R = "/opt/dlami/nvme/dfperf/runs/op1a"
def runs(label, arm, q):
    out = []
    for d in sorted(glob.glob(f"{R}/{label}/{arm}/tpch_*_q{q}")):
        try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception: continue
        t = {int(r["iteration"]): float(r["runtime_s"]) * 1000 for r in rows}
        lg = glob.glob(f"{d}/log_dir/sirius_*.log")
        txt = open(lg[0], errors="replace").read() if lg else ""
        out.append(dict(dir=d, t=t, hot=[t[i] for i in sorted(t) if i >= 2], txt=txt))
    return out
args = sys.argv[1:]
PA, PB = "A", "B"
if args and args[0].startswith("--arms="):
    PA, PB = args[0][7:].split(","); args = args[1:]
labels = args
def arms_of(label, prefix):
    return sorted({os.path.basename(d) for d in glob.glob(f"{R}/{label}/{prefix}*")})
qs = sorted({int(re.search(r"_q(\d+)$", d).group(1)) for l in labels for d in glob.glob(f"{R}/{l}/*/tpch_*_q*")})
print("| q | tip med ms (n) [min-max] | proto med ms (n) [min-max] | delta ms | delta % | paired delta ms (per pair) | per-process medians tip | proto | results identical | flags |")
print("|---|---|---|---|---|---|---|---|---|---|")
tot_a = tot_b = 0
PAIRED = {}
for q in qs:
    A = [r for l in labels for arm in arms_of(l, PA) for r in runs(l, arm, q)]
    B = [r for l in labels for arm in arms_of(l, PB) for r in runs(l, arm, q)]
    if not A or not B: continue
    a = [x for r in A for x in r["hot"]]; b = [x for r in B for x in r["hot"]]
    if not a or not b: continue
    ma, mb = st.median(a), st.median(b)
    res = [open(f"{r['dir']}/sirius/q{q}/result.txt", errors="replace").read() if os.path.exists(f"{r['dir']}/sirius/q{q}/result.txt") else None for r in A + B]
    same = all(x is not None and x == res[0] for x in res)
    flags = []
    for name, rs in ((PA, A), (PB, B)):
        txt = "".join(r["txt"] for r in rs)
        fb = len(re.findall(r"execution fallback|fallback to DuckDB|falling back", txt, re.I))
        er = len(re.findall(r"Error in Sirius|bad_alloc|out of memory|illegal memory|\[error\]", txt))
        rt = txt.count("reschedule (retry")
        c = txt.count("construction started"); p = txt.count("constructed filters published")
        flags.append(f"{name}: started/published {c}/{p}" + (f" fb={fb}" if fb else "") + (f" err={er}" if er else "") + (f" retry={rt}" if rt else ""))
    pm = lambda rs: " ".join(f"{st.median(r['hot']):.0f}" for r in rs if r["hot"])
    # paired: per label, (A1,B1) and (A2,B2) process medians; delta = mean of B - A over pairs
    pd = []
    for l in labels:
        for i in ("1", "2"):
            ra = runs(l, PA + i, q); rb = runs(l, PB + i, q)
            if ra and rb and ra[-1]["hot"] and rb[-1]["hot"]:
                pd.append(st.median(rb[-1]["hot"]) - st.median(ra[-1]["hot"]))
    paired = (sum(pd) / len(pd)) if pd else float("nan")
    PAIRED[q] = (paired, pd, ma)
    print(f"| {q} | {ma:.1f} ({len(a)}) [{min(a):.0f}-{max(a):.0f}] | {mb:.1f} ({len(b)}) [{min(b):.0f}-{max(b):.0f}] | {mb-ma:+.1f} | {100*(mb-ma)/ma:+.2f} | {paired:+.1f} ({' '.join(f'{x:+.0f}' for x in pd)}) | {pm(A)} | {pm(B)} | {'yes' if same else 'NO'} | {'; '.join(flags)} |")
    tot_a += ma; tot_b += mb
ps = sum(v[0] for v in PAIRED.values() if v[0] == v[0])
print(f"| sum | {tot_a:.0f} | {tot_b:.0f} | {tot_b-tot_a:+.0f} | {100*(tot_b-tot_a)/tot_a:+.2f} | {ps:+.0f} ({100*ps/tot_a:+.2f}%) | | | | |")
