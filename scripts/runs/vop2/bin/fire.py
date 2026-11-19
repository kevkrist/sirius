#!/usr/bin/env python3
"""fire.py <label_dir> [q ...]: per window (iteration) of each query's INFO log: constructions started / published, HOST-staged
publications and 'ms after the final contribution' values. Shows whether a skip happened in every iteration."""
import sys, glob, re
base = sys.argv[1]; qs = sys.argv[2:]
for d in sorted(glob.glob(f"{base}/tpch_*_q*")):
    q = re.search(r"_q(\d+)$", d).group(1)
    if qs and q not in qs: continue
    for lf in sorted(glob.glob(f"{d}/log_dir/sirius_*.log")):
        w = []; cur = None
        for l in open(lf, errors="replace"):
            if "[window] begin" in l: cur = dict(started=0, published=0, host=0, ms=[], keys=0); w.append(cur)
            elif cur is None: continue
            elif "construction started" in l: cur["started"] += 1; m = re.search(r"(\d+) key\(s\)", l); cur["keys"] += int(m.group(1)) if m else 0
            elif "constructed filters published" in l:
                cur["published"] += 1; cur["host"] += "HOST staging" in l
                m = re.search(r"([\d.]+) ms after", l); cur["ms"].append(float(m.group(1)) if m else -1)
        w = [x for x in w if x["started"] or x["published"]] or w[-6:]
        print(f"q{q} {d.split('/')[-2]}: " + " | ".join(f"s{x['started']}k{x['keys']}p{x['published']}h{x['host']} max={max(x['ms'] or [0]):.0f}" for x in w))
