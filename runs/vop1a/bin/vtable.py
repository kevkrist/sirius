#!/usr/bin/env python3
"""vtable.py <runs root> <armA prefix> <armB prefix> <label> [<label> ...]
Independent A/B table (verifier). Per process: hot = iterations >= 2 (iteration 0 warm, 1 = first hot dropped).
Reports per query: pooled medians, per-process medians, paired deltas (X1 vs Y1, X2 vs Y2 per label),
the same with the first hot iteration kept (robustness), result identity, DF counters, error lines.
"""
import csv, glob, os, re, statistics as st, sys

root, PA, PB = sys.argv[1], sys.argv[2], sys.argv[3]
labels = sys.argv[4:]


def runs(label, arm, q):
    out = []
    for d in sorted(glob.glob(f"{root}/{label}/{arm}/tpch_*_q{q}")):
        try:
            rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception:
            continue
        t = {int(r["iteration"]): float(r["runtime_s"]) * 1000 for r in rows}
        lg = glob.glob(f"{d}/log_dir/sirius_*.log")
        txt = "".join(open(f, errors="replace").read() for f in lg)
        res = f"{d}/sirius/q{q}/result.txt"
        out.append(
            dict(
                dir=d,
                t=t,
                hot=[t[i] for i in sorted(t) if i >= 2],
                hot1=[t[i] for i in sorted(t) if i >= 1],
                txt=txt,
                res=open(res, errors="replace").read() if os.path.exists(res) else None,
            )
        )
    return out


def arms_of(label, prefix):
    return sorted({os.path.basename(d) for d in glob.glob(f"{root}/{label}/{prefix}*") if os.path.isdir(d)})


qs = sorted({int(re.search(r"_q(\d+)$", d).group(1)) for l in labels for d in glob.glob(f"{root}/{l}/*/tpch_*_q*")})
print(f"| q | {PA} med (n) [min-max] | {PB} med (n) [min-max] | pooled d ms | % | paired d ms (pairs) | paired, 1st hot kept | proc medians {PA} | {PB} | result | sorted result | counters / errors |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
ta = tb = tp = 0.0
for q in qs:
    A = [r for l in labels for arm in arms_of(l, PA) for r in runs(l, arm, q)]
    B = [r for l in labels for arm in arms_of(l, PB) for r in runs(l, arm, q)]
    a = [x for r in A for x in r["hot"]]
    b = [x for r in B for x in r["hot"]]
    if not a or not b:
        continue
    ma, mb = st.median(a), st.median(b)
    res = [r["res"] for r in A + B]
    same = all(x is not None and x == res[0] for x in res)
    ssame = all(x is not None and sorted(x.splitlines()) == sorted(res[0].splitlines()) for x in res)
    flags = []
    for name, rs in ((PA, A), (PB, B)):
        txt = "".join(r["txt"] for r in rs)
        fb = len(re.findall(r"execution fallback|fallback to DuckDB|falling back", txt, re.I))
        er = len(re.findall(r"Error in Sirius|bad_alloc|out of memory|illegal memory|\[error\]", txt))
        rt = txt.count("reschedule (retry")
        c = txt.count("construction started")
        p = txt.count("constructed filters published")
        hs = txt.count("through HOST staging")
        ca = txt.count("[df_async] continuation")
        fl = len(re.findall(r"\[df_async\] continuation.*failed=1", txt))
        flags.append(f"{name}: start/pub {c}/{p} host-staged {hs} cont {ca} failed {fl} fb {fb} err {er} retry {rt}")
    pd, pd1 = [], []
    for l in labels:
        for i in ("1", "2"):
            ra, rb = runs(l, PA + i, q), runs(l, PB + i, q)
            if ra and rb and ra[-1]["hot"] and rb[-1]["hot"]:
                pd.append(st.median(rb[-1]["hot"]) - st.median(ra[-1]["hot"]))
                pd1.append(st.median(rb[-1]["hot1"]) - st.median(ra[-1]["hot1"]))
    paired = sum(pd) / len(pd) if pd else float("nan")
    paired1 = sum(pd1) / len(pd1) if pd1 else float("nan")
    pm = lambda rs: " ".join(f"{st.median(r['hot']):.1f}" for r in rs if r["hot"])
    print(
        f"| {q} | {ma:.1f} ({len(a)}) [{min(a):.0f}-{max(a):.0f}] | {mb:.1f} ({len(b)}) [{min(b):.0f}-{max(b):.0f}] | {mb-ma:+.1f} | {100*(mb-ma)/ma:+.2f} "
        f"| {paired:+.1f} ({' '.join(f'{x:+.1f}' for x in pd)}) | {paired1:+.1f} | {pm(A)} | {pm(B)} | {'same' if same else 'DIFF'} | {'same' if ssame else 'DIFF'} | {'; '.join(flags)} |"
    )
    ta += ma
    tb += mb
    tp += paired if paired == paired else 0.0
print(f"| sum | {ta:.0f} | {tb:.0f} | {tb-ta:+.0f} | {100*(tb-ta)/ta if ta else 0:+.2f} | {tp:+.0f} | | | | | | |")
