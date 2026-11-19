#!/usr/bin/env python3
"""vtable.py [tag ...]: per query, per variant vs tip from runs/vop2/<tag>_<variant>_<pos>.
Hot = iterations >= 2 (iter0 warm, iter1 first hot dropped). Per-block delta = med(variant in tag) - med(tip in tag)."""
import csv, glob, statistics as st, sys, re, os, hashlib, collections
B = "/opt/dlami/nvme/dfperf/runs/vop2"
tags = sys.argv[1:] or sorted({os.path.basename(d).split("_")[0] for d in glob.glob(f"{B}/*_*_*") if os.path.isdir(d) and not os.path.basename(d).startswith("dbg")})
data = collections.defaultdict(lambda: collections.defaultdict(lambda: collections.defaultdict(list)))  # q -> tag -> variant -> [samples]
hashes = collections.defaultdict(lambda: collections.defaultdict(set)); firsthot = collections.defaultdict(lambda: collections.defaultdict(list))
for t in tags:
    for lab in sorted(glob.glob(f"{B}/{t}_*_*")):
        m = re.match(rf"{t}_(\w+?)_(\d+)$", os.path.basename(lab))
        if not m: continue
        v = m.group(1)
        for d in sorted(glob.glob(f"{lab}/tpch_*_q*")):
            q = int(re.search(r"_q(\d+)$", d).group(1))
            try: rows = list(csv.DictReader(open(f"{d}/csv/runtimes.csv")))
            except Exception: continue
            tt = {int(r["iteration"]): float(r["runtime_s"]) * 1000 for r in rows}
            if len(tt) < 3: continue
            data[q][t][v] += [tt[i] for i in sorted(tt) if i >= 2]
            firsthot[q][v].append(tt.get(1))
            res = f"{d}/sirius/q{q}/result.txt"
            hashes[q][v].add(hashlib.md5(open(res, "rb").read()).hexdigest()[:8] if os.path.exists(res) else "MISSING")
print("| q | variant | tip med (n) [min-max] | variant med (n) [min-max] | delta ms | delta % | per-block deltas | result vs tip |")
print("|---|---|---|---|---|---|---|---|")
for q in sorted(data):
    variants = sorted({v for t in data[q] for v in data[q][t]} - {"tip"})
    for v in variants:
        tip, var, blocks = [], [], []
        for t in sorted(data[q]):
            if v not in data[q][t] or "tip" not in data[q][t]: continue
            tip += data[q][t]["tip"]; var += data[q][t][v]
            blocks.append(f"{t}:{st.median(data[q][t][v]) - st.median(data[q][t]['tip']):+.1f}")
        if not tip: continue
        mt, mv = st.median(tip), st.median(var)
        same = "identical" if hashes[q][v] == hashes[q]["tip"] and len(hashes[q][v]) == 1 else f"DIFF {hashes[q][v]} vs {hashes[q]['tip']}"
        print(f"| {q} | {v} | {mt:.1f} ({len(tip)}) [{min(tip):.0f}-{max(tip):.0f}] | {mv:.1f} ({len(var)}) [{min(var):.0f}-{max(var):.0f}] | {mv-mt:+.1f} | {100*(mv-mt)/mt:+.2f} | {' '.join(blocks)} | {same} |")
