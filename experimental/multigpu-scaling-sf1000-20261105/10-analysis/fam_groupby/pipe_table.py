#!/usr/bin/env python3
"""pipe_table.py <quent_dir> <query_label> : per-pipeline timeline + thread-time for one query iteration.
Columns: pipeline label, ntasks, start/end offsets from query init (ms, first preparing -> last finalize),
prep thread-ms split HOST/GPU origin, compute thread-ms per op, input GB, bytes_to_materialize not available here (req_bytes)."""
import csv, sys, collections
d, q = sys.argv[1], sys.argv[2]
qs = {r['query']: r for r in csv.DictReader(open(d + '/queries.csv'))}
q0 = int(qs[q]['init']); wall = float(qs[q]['wall_ms'])
pl = {r['pipeline']: (r['type_name'], r['label']) for r in csv.DictReader(open(d + '/pipelines.csv')) if r['query'] == q}
T = [r for r in csv.DictReader(open(d + '/tasks.csv')) if r['query'] == q]
O = collections.defaultdict(list)
for r in csv.DictReader(open(d + '/task_ops.csv')):
    if r['query'] == q: O[r['task']].append(r)
agg = collections.defaultdict(lambda: dict(n=0, s=float('inf'), e=0, ph=0, pg=0, inb=0, comp=collections.Counter(), gpus=collections.Counter(), task=0, maxprep=0, preps=[]))
tot = collections.Counter()
for t in T:
    p = pl.get(t['pipeline'], ('?', '?'))
    a = agg[p]
    a['n'] += 1; a['gpus'][t['gpu']] += 1
    pr = int(t['preparing']) if t['preparing'] else None
    fc = int(t['first_compute']) if t['first_compute'] else None
    fz = int(t['finalize']) if t['finalize'] else None
    if pr: a['s'] = min(a['s'], pr)
    if fz: a['e'] = max(a['e'], fz)
    if pr and fc:
        x = (fc - pr) / 1e6
        a['preps'].append(x); a['maxprep'] = max(a['maxprep'], x)
        if t['origin'] == 'HOST': a['ph'] += x; tot['prep_host'] += x
        else: a['pg'] += x; tot['prep_gpu'] += x
    if pr and fz: a['task'] += (fz - pr) / 1e6; tot['task'] += (fz - pr) / 1e6
    a['inb'] += int(t['in_bytes'] or 0)
    for o in O[t['task']]:
        if o['end']:
            x = (int(o['end']) - int(o['start'])) / 1e6
            a['comp'][o['operator_id']] += x; tot['comp'] += x
print(f"## {q}  wall={wall:.0f} ms  thread-ms task={tot['task']:.0f} prepHOST={tot['prep_host']:.0f} prepGPU={tot['prep_gpu']:.0f} comp={tot['comp']:.0f}")
print(f"{'pipeline':58} {'n':>5} {'start':>7} {'end':>7} {'prepH':>7} {'prepG':>7} {'maxprep':>7} {'compute per op (thread-ms)':30} {'inGB':>6} gpus")
for p, a in sorted(agg.items(), key=lambda kv: kv[1]['s']):
    comp = ' '.join(f"{k}:{v:.0f}" for k, v in sorted(a['comp'].items(), key=lambda kv: int(kv[0])))
    print(f"{(p[0].replace('Pipeline Id ','P')+' '+p[1])[:58]:58} {a['n']:5} {(a['s']-q0)/1e6:7.0f} {(a['e']-q0)/1e6:7.0f} {a['ph']:7.0f} {a['pg']:7.0f} {a['maxprep']:7.1f} {comp:30} {a['inb']/1e9:6.2f} {dict(a['gpus'])}")
