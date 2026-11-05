#!/usr/bin/env python3
"""scan_phases.py <host_quent> <gpupin_quent> <iters> : per query, scan-pipeline phase durations (first prep -> last end)
host-pinned vs gpu-pinned, interior non-scan gaps between scan phases (host run), and the lookahead-reachable bound
  R_q = min(sum interior gaps, sum over scans after the first interior gap of max(0, dur_host - dur_gpupin)).
Matching is by pipeline label. Values are means over the given iters (ms, un-profiled)."""
import csv, sys
from collections import defaultdict
def load(qd):
    pl = {r['pipeline']: (r['query'], r['label']) for r in csv.DictReader(open(f"{qd}/pipelines.csv"))}
    Q = {r['query']: int(r['executing']) for r in csv.DictReader(open(f"{qd}/queries.csv"))}
    W = {r['query']: float(r['wall_ms']) for r in csv.DictReader(open(f"{qd}/queries.csv"))}
    P = defaultdict(lambda: [1e30, 0])
    for r in csv.DictReader(open(f"{qd}/tasks.csv")):
        if r['pipeline'] not in pl: continue
        q, lab = pl[r['pipeline']]
        if not lab.startswith('GPU_SCAN'): continue
        d = P[(q, lab)]
        d[0] = min(d[0], int(r['preparing'] or r['created'])); d[1] = max(d[1], int(r['finalize'] or 0))
    out = defaultdict(list)
    for (q, lab), (s, e) in P.items(): out[q].append((lab, (s - Q[q]) / 1e6, (e - Q[q]) / 1e6))
    for q in out: out[q].sort(key=lambda x: x[1])
    return out, W
H, WH = load(sys.argv[1]); G, WG = load(sys.argv[2]); iters = sys.argv[3].split(',')
tot = defaultdict(float)
print("Q  wallH wallG  scanH scanG  gaps>=5  laterExcess  R(reach)")
for qn in range(1, 23):
    acc = defaultdict(float)
    for it in iters:
        lab = f"tpch_q{qn}_iter{it}"
        if lab not in H or lab not in G: continue
        h = H[lab]; g = {l: e - s for l, s, e in G[lab]}
        # merge overlapping scan phases into a union and find interior gaps
        gaps = []; cur_end = h[0][2]; first_gap_end = None
        for l, s, e in h[1:]:
            if s - cur_end >= 5:
                gaps.append(s - cur_end)
                if first_gap_end is None: first_gap_end = s
            cur_end = max(cur_end, e)
        later = sum(max(0, (e - s) - g.get(l, e - s)) for l, s, e in h if first_gap_end is not None and s >= first_gap_end)
        acc['wallH'] += WH[lab]; acc['wallG'] += WG[lab]
        acc['scanH'] += sum(e - s for l, s, e in h); acc['scanG'] += sum(g.values())
        acc['gaps'] += sum(gaps); acc['later'] += later; acc['R'] += min(sum(gaps), later); acc['n'] += 1
    n = acc.pop('n', 0)
    if not n: continue
    v = {k: x / n for k, x in acc.items()}
    for k in v: tot[k] += v[k]
    print(f"{qn:<3}{v['wallH']:6.0f}{v['wallG']:6.0f}{v['scanH']:7.0f}{v['scanG']:6.0f}{v['gaps']:8.0f}{v['later']:12.0f}{v['R']:9.0f}")
print({k: round(x) for k, x in tot.items()})
