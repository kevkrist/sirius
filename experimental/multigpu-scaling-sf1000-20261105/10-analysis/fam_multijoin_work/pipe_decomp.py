#!/usr/bin/env python3
"""pipe_decomp.py <quentA> <quentB> <qnum> [iters=2,3,4]
Per pipeline (matched by 'Pipeline Id K' + label) of TPC-H query qnum, averaged over the listed hot iterations:
tasks, HOST-prep ms, GPU-prep ms, per-operator compute ms (thread-time), in_bytes GB, pipeline span (first preparing
-> last finalize, ms from query start). A=g1, B=g4."""
import csv, sys, collections
A, B, qn = sys.argv[1], sys.argv[2], sys.argv[3]
iters = [int(x) for x in (sys.argv[4] if len(sys.argv) > 4 else '2,3,4').split(',')]
def load(d):
    qs = {f'tpch_q{qn}_iter{i}' for i in iters}
    qstart = {}
    for r in csv.DictReader(open(d + '/queries.csv')) if False else []: pass
    rows = list(csv.reader(open(d + '/queries.csv')))
    hdr = rows[0]
    for r in rows[1:]:
        if r[0] in qs: qstart[r[0]] = (int(r[2]), int(r[3]), float(r[4]))
    pl = {}
    for r in csv.DictReader(open(d + '/pipelines.csv')):
        if r['query'] in qs: pl[r['pipeline']] = (r['query'], r['type_name'].replace('Pipeline Id ', 'P'), r['label'])
    agg = collections.defaultdict(collections.Counter)
    span = collections.defaultdict(lambda: [1e30, 0])
    ops = collections.defaultdict(collections.Counter)
    t2p = {}
    for r in csv.DictReader(open(d + '/tasks.csv')):
        if r['pipeline'] not in pl: continue
        q, pid, lab = pl[r['pipeline']]; k = (pid, lab); t2p[r['task']] = k
        try: p = int(r['preparing']); f = int(r['first_compute']); e = int(r['finalize'])
        except: continue
        a = agg[k]; a['n'] += 1; a['prep_' + r['origin']] += (f - p) / 1e6; a['comp'] += (e - f) / 1e6
        a['bytes_' + r['origin']] += int(r['in_bytes'] or 0)
        qs0 = qstart[q][0]
        s = span[(k, q)]; s[0] = min(s[0], (p - qs0) / 1e6); s[1] = max(s[1], (e - qs0) / 1e6)
    for r in csv.DictReader(open(d + '/task_ops.csv')):
        k = t2p.get(r['task'])
        if k and r['end']: ops[k][r['operator_id']] += (int(r['end']) - int(r['start'])) / 1e6
    n = len(iters)
    sp = collections.defaultdict(lambda: [0, 0])
    for (k, q), s in span.items(): sp[k][0] += s[0] / n; sp[k][1] += s[1] / n
    wall = sum(v[2] for v in qstart.values()) / n
    return agg, ops, sp, wall, n
aA, oA, sA, wA, n = load(A); aB, oB, sB, wB, _ = load(B)
print(f"wall g1={wA:.0f} g4={wB:.0f} ms  (quent, mean iters {iters})")
print(f"{'pipe':5}{'label':52}{'n1':>6}{'n4':>6}{'pH1':>7}{'pH4':>7}{'pG1':>7}{'pG4':>7}{'c1':>7}{'c4':>7}{'GBin1':>7}{'GBin4':>7}{'span1':>12}{'span4':>12}")
T = collections.Counter()
keys = sorted(set(aA) | set(aB), key=lambda k: int(k[0][1:]))
for k in keys:
    a, b = aA[k], aB[k]
    v = [a['n'] / n, b['n'] / n, a['prep_HOST'] / n, b['prep_HOST'] / n, a['prep_GPU'] / n, b['prep_GPU'] / n, a['comp'] / n, b['comp'] / n]
    for nm, x in zip(['n1', 'n4', 'pH1', 'pH4', 'pG1', 'pG4', 'c1', 'c4'], v): T[nm] += x
    gb1 = (a['bytes_HOST'] + a['bytes_GPU']) / n / 1e9; gb4 = (b['bytes_HOST'] + b['bytes_GPU']) / n / 1e9
    s1 = sA.get(k, [0, 0]); s4 = sB.get(k, [0, 0])
    print(f"{k[0]:5}{k[1][:52]:52}{v[0]:6.0f}{v[1]:6.0f}{v[2]:7.0f}{v[3]:7.0f}{v[4]:7.0f}{v[5]:7.0f}{v[6]:7.0f}{v[7]:7.0f}{gb1:7.1f}{gb4:7.1f}{s1[0]:6.0f}-{s1[1]:<5.0f}{s4[0]:6.0f}-{s4[1]:<5.0f}")
print('TOTAL', ' '.join(f"{k}={v:.0f}" for k, v in T.items()), f"thread g1={T['pH1']+T['pG1']+T['c1']:.0f} g4={T['pH4']+T['pG4']+T['c4']:.0f}",
      f"occ g1={(T['pH1']+T['pG1']+T['c1'])/wA/4:.3f} g4={(T['pH4']+T['pG4']+T['c4'])/wB/16:.3f}")
print("\nper-operator compute (thread-ms)")
opsum = collections.defaultdict(lambda: [0, 0])
for k in keys:
    for oid in set(oA[k]) | set(oB[k]):
        name = next((s.split('(')[0] for s in k[1].split(' -> ') if s.endswith(f'({oid})')), '?')
        x1, x4 = oA[k][oid] / n, oB[k][oid] / n
        opsum[name][0] += x1; opsum[name][1] += x4
        if max(x1, x4) > 20: print(f"  {k[0]:5}{name}({oid}){'':5}{x1:8.0f}{x4:8.0f}{(x4/x1 if x1 else 0):6.2f}")
print("by op type:", ', '.join(f"{k} {v[0]:.0f}->{v[1]:.0f}" for k, v in sorted(opsum.items(), key=lambda kv: -kv[1][1])))
