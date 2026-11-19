#!/usr/bin/env python3
"""table.py <tag> [q...]: ABBA table. Hot = iterations >= 2 (iter0 warm, iter1 dropped). Medians over both blocks of a side."""
import csv, glob, re, statistics as st, sys, hashlib
B = "/opt/dlami/nvme/dfperf/runs/op4"
tag = sys.argv[1]; qs = [int(x) for x in sys.argv[2:]] or range(1, 23)
def load(label, q):
    ds = sorted(glob.glob(f"{B}/{label}/tpch_*_q{q}"))
    if not ds: return None
    out = []
    for d in ds:
        t = {}
        try:
            for r in csv.DictReader(open(f"{d}/csv/runtimes.csv")):
                if r.get("query") in (f"q{q}", str(q)): t[int(r["iteration"])] = float(r["runtime_s"]) * 1000
        except Exception: continue
        hot = [t[i] for i in sorted(t) if i >= 2]
        lg = glob.glob(f"{d}/log_dir/sirius_*.log"); txt = open(lg[0], errors="replace").read() if lg else ""
        try: res = hashlib.md5(open(f"{d}/sirius/q{q}/result.txt", "rb").read()).hexdigest()[:8]
        except Exception: res = "none"
        bad = len(re.findall(r"execution fallback|fallback to DuckDB|falling back|Error in Sirius|bad_alloc|out of memory", txt))
        if len(hot) >= 2: out.append(dict(hot=hot, res=res, bad=bad, constr=txt.count("construction started")))
    return out or None
print("| q | A med ms | B med ms | B-A ms | B-A % | A blocks (med) | B blocks (med) | A spread | B spread | result A/B | constr A/B | flags |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
ta = tb = 0
for q in qs:
    A = [x for l in ("A1", "A2") for x in (load(f"{tag}_{l}", q) or [])]
    Bv = [x for l in ("B1", "B2") for x in (load(f"{tag}_{l}", q) or [])]
    if not A or not Bv: print(f"| {q} | incomplete |"); continue
    ah = [v for x in A for v in x["hot"]]; bh = [v for x in Bv for v in x["hot"]]
    am, bm = st.median(ah), st.median(bh); ta += am; tb += bm
    ra = {x["res"] for x in A}; rb = {x["res"] for x in Bv}
    flags = ("RESULT-DIFF " if ra != rb else "") + ("BAD " if any(x["bad"] for x in A + Bv) else "")
    print(f"| {q} | {am:.0f} | {bm:.0f} | {bm-am:+.1f} | {(bm/am-1)*100:+.1f} | {'/'.join(f'{st.median(x['hot']):.0f}' for x in A)} | {'/'.join(f'{st.median(x['hot']):.0f}' for x in Bv)} | {max(ah)-min(ah):.0f} | {max(bh)-min(bh):.0f} | {','.join(sorted(ra))} / {','.join(sorted(rb))} | {A[0]['constr']}/{Bv[0]['constr']} | {flags} |")
print(f"| sum | {ta:.0f} | {tb:.0f} | {tb-ta:+.1f} | {(tb/ta-1)*100:+.2f} | | | | | | | |")
