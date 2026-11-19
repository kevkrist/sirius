#!/usr/bin/env python3
"""abba_table.py <tag> [<tag> ...]: per-query tip vs proto from runs/op2/<tag>_{tipA,protoA,protoB,tipB}.
Hot = iterations >= 2 (iter0 warm, iter1 = first hot, dropped). Pools all tags given. Also diffs result.txt."""
import csv, glob, statistics as st, sys, re, os, hashlib
B = "/opt/dlami/nvme/dfperf/runs/op2"
def runs(label, q):
    out = []
    for d in sorted(glob.glob(f"{B}/{label}/tpch_*_q{q}")):
        try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
        except Exception: continue
        t = {int(r["iteration"]): float(r["runtime_s"]) for r in rows if r.get("query") in (f"q{q}", str(q))}
        res = f"{d}/sirius/q{q}/result.txt"
        h = hashlib.md5(open(res, "rb").read()).hexdigest()[:8] if os.path.exists(res) else None
        out.append(([t[i] * 1000 for i in sorted(t) if i >= 2], h))
    return out
tags = sys.argv[1:]
qs = sorted({int(re.search(r"_q(\d+)$", d).group(1)) for t in tags for d in glob.glob(f"{B}/{t}_tipA/tpch_*_q*")})
print("| q | tip med ms (n) [min-max] | proto med ms (n) [min-max] | delta ms | delta % | per-block deltas ms | results |")
print("|---|---|---|---|---|---|---|")
tt = tp = 0
for q in qs:
    tip, pro, blocks, hs = [], [], [], set()
    for t in tags:
        ta, pa, pb, tb = (runs(f"{t}_{s}", q) for s in ("tipA", "protoA", "protoB", "tipB"))
        for i in range(min(len(ta), len(pa), len(pb), len(tb))):
            bt = ta[i][0] + tb[i][0]; bp = pa[i][0] + pb[i][0]
            if not bt or not bp: continue
            tip += bt; pro += bp; blocks.append(st.median(bp) - st.median(bt))
            hs |= {("tip", ta[i][1]), ("tip", tb[i][1]), ("proto", pa[i][1]), ("proto", pb[i][1])}
    if not tip: continue
    mt, mp = st.median(tip), st.median(pro); tt += mt; tp += mp
    same = "identical" if len({h for _, h in hs}) == 1 else f"DIFF {sorted(hs)}"
    print(f"| {q} | {mt:.1f} ({len(tip)}) [{min(tip):.0f}-{max(tip):.0f}] | {mp:.1f} ({len(pro)}) [{min(pro):.0f}-{max(pro):.0f}] | {mp-mt:+.1f} | {100*(mp-mt)/mt:+.2f} | {' '.join(f'{b:+.1f}' for b in blocks)} | {same} |")
print(f"| sum | {tt:.0f} | {tp:.0f} | {tp-tt:+.1f} | {100*(tp-tt)/tt:+.2f} | | |")
