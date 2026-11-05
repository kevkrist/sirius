#!/usr/bin/env python3
"""task_list.py <quent_dir> <query_label> <pipeline label substring> : per-task timeline (ms from query init)."""
import csv, sys, collections
d, q, sub = sys.argv[1:4]
qs = {r['query']: r for r in csv.DictReader(open(d + '/queries.csv'))}
q0 = int(qs[q]['init'])
pl = {r['pipeline']: r['label'] for r in csv.DictReader(open(d + '/pipelines.csv')) if r['query'] == q and sub in r['label']}
O = collections.defaultdict(list)
for r in csv.DictReader(open(d + '/task_ops.csv')):
    if r['query'] == q: O[r['task']].append(r)
f = lambda x: (int(x) - q0) / 1e6 if x else float('nan')
rows = [t for t in csv.DictReader(open(d + '/tasks.csv')) if t['query'] == q and t['pipeline'] in pl]
rows.sort(key=lambda t: int(t['preparing'] or 0))
print("gpu thread created routed reserving preparing first_compute finalize prep comp origin in_MB req_MB ops")
for t in rows:
    ops = ' '.join(f"{o['operator_id']}:{(int(o['end'])-int(o['start']))/1e6:.1f}" for o in O[t['task']] if o['end'])
    print(t['gpu'], t['thread'].split('-')[-1], *(f"{f(t[k]):.1f}" for k in ('created','routed','reserving','preparing','first_compute','finalize')),
          f"{f(t['first_compute'])-f(t['preparing']):.1f}", f"{f(t['finalize'])-f(t['first_compute']):.1f}", t['origin'], f"{int(t['in_bytes'] or 0)/1e6:.0f}", f"{int(t['req_bytes'] or 0)/1e6:.0f}", ops)
