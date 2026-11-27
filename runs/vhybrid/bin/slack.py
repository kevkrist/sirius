#!/usr/bin/env python3
# Per join: one-shot filter-ready time (claimant line) vs last whole-build slot deposit (= default probe release).
# usage: slack.py <label dir> q...
import sys, re, glob, statistics as st
from datetime import datetime
d = sys.argv[1]
def ts(l): return datetime.strptime(l[1:24], "%Y-%m-%d %H:%M:%S.%f").timestamp()*1000
for q in sys.argv[2:]:
    fs = sorted(glob.glob(f"{d}/*_q{q}/log_dir/sirius_*.log"))
    if not fs: continue
    wins = []; cur = None
    for l in open(fs[-1], errors="replace"):
        if "[window] begin" in l: cur = {"t0": ts(l), "j": {}, "defer": {}}
        elif cur is None: continue
        elif "[window] end" in l: wins.append(cur); cur = None
        elif "defers construction" in l:
            m = re.search(r"join (\d+) defers", l); cur["defer"][int(m.group(1))] = ts(l) - cur["t0"]
        elif "whole-build delivery slot" in l:
            m = re.search(r"join (\d+) whole-build delivery slot (\d+) observed in ([\d.]+) ms", l)
            cur["j"].setdefault(int(m.group(1)), []).append((ts(l) - cur["t0"], float(m.group(3))))
    wins = wins[2:]  # drop warm + first hot
    joins = sorted({j for w in wins for j in w["j"]})
    for j in joins:
        rows = []
        for w in wins:
            if j not in w["j"]: continue
            ev = w["j"][j]; claim = max(ev, key=lambda e: e[1])
            ready = claim[0]; last = max(e[0] for e in ev); first_arr = min(e[0] - e[1] for e in ev)
            rows.append((w["defer"].get(j, float("nan")), first_arr, claim[0]-claim[1], claim[1], ready, last, last - ready))
        med = [st.median(c) for c in zip(*rows)]
        if med[3] < 1.0 and med[6] < 3: continue
        print(f"q{q} j{j} n={len(rows)} part_end={med[0]:.0f} first_concat={med[1]:.0f} claim_start={med[2]:.0f} oneshot={med[3]:.1f} ready={med[4]:.0f} last_deposit={med[5]:.0f} avoidable={med[6]:.0f}")
