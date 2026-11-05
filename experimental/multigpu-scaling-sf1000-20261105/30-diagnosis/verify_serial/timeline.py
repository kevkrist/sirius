#!/usr/bin/env python3
"""timeline.py <quent_dir> <label>: per pipeline first-created / first-prepare / last-finalize (ms from query exec start)."""
import csv, sys
from collections import defaultdict
qd, lab = sys.argv[1], sys.argv[2]
pl = {r['pipeline']: r['label'] for r in csv.DictReader(open(f"{qd}/pipelines.csv")) if r['query'] == lab}
q = [r for r in csv.DictReader(open(f"{qd}/queries.csv")) if r['query'] == lab][0]
t0 = int(q.get('executing') or q.get('init'))
P = defaultdict(lambda: dict(n=0, c=1e30, p=1e30, f=0, host=0, hb=0))
for r in csv.DictReader(open(f"{qd}/tasks.csv")):
    if r['query'] != lab: continue
    d = P[r['pipeline']]; d['n'] += 1
    d['c'] = min(d['c'], int(r['created'])); d['p'] = min(d['p'], int(r['preparing'] or r['created']))
    d['f'] = max(d['f'], int(r['finalize'] or 0))
    if r['origin'] == 'HOST': d['host'] += 1; d['hb'] += int(r['in_bytes'] or 0)
print(f"{lab} wall {(int(q['finished'])-t0)/1e6:.0f} ms")
for k, d in sorted(P.items(), key=lambda kv: kv[1]['c']):
    print(f"  created {(d['c']-t0)/1e6:7.0f} prep {(d['p']-t0)/1e6:7.0f} end {(d['f']-t0)/1e6:7.0f} n={d['n']:4d} host={d['host']:4d} {d['hb']/1e9:6.1f}GB  {pl.get(k,'?')[:110]}")
