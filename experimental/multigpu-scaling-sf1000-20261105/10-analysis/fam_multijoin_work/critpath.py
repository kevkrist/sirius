#!/usr/bin/env python3
"""critpath.py <quentA=g1> <quentB=g4> <qnum> [iters]: attribute each ms of query wall to ONE pipeline: among pipelines
with a task in flight (preparing..finalize), the most-upstream (lowest pipeline id) one; idle gaps -> 'gap'.
Prints per pipeline crit-ms A, B, ideal=A/4, excess=B-A/4 (mean over iters)."""
import csv, sys, collections
A, B, qn = sys.argv[1], sys.argv[2], sys.argv[3]
iters = [int(x) for x in (sys.argv[4] if len(sys.argv) > 4 else '2,3,4').split(',')]
def load(d):
    Q = {r[0]: (int(r[2]), int(r[3])) for r in csv.reader(open(d + '/queries.csv')) if r[0].startswith('tpch')}
    lab = {r['pipeline']: r['label'] for r in csv.DictReader(open(d + '/pipelines.csv'))}
    iv = collections.defaultdict(list)
    for r in csv.DictReader(open(d + '/tasks.csv')):
        if not any(r['query'] == f'tpch_q{qn}_iter{i}' for i in iters): continue
        try: p = int(r['preparing']); e = int(r['finalize'])
        except: continue
        pid = int(r['pipeline_type'].split()[-1])
        iv[r['query']].append((p, e, pid, lab.get(r['pipeline'], '?')))
    res = collections.Counter(); labels = {}
    for q, L in iv.items():
        s, e = Q[q]; step = 100_000  # 0.1 ms
        # event sweep
        ev = []
        for p, f, pid, l in L: ev.append((p, 1, pid)); ev.append((f, -1, pid)); labels[pid] = l
        ev.sort(); active = collections.Counter(); t = s
        import heapq
        for tt, d, pid in ev:
            tt = max(tt, s)
            if tt > t:
                live = [k for k, v in active.items() if v > 0]
                res[min(live) if live else 'gap'] += (tt - t) / 1e6 / len(iters)
                t = tt
            active[pid] += d
        if e > t: res['gap'] += (e - t) / 1e6 / len(iters)
    return res, labels
ra, la = load(A); rb, lb = load(B)
lab = {**la, **lb}
print(f"{'pipe':6}{'label':55}{'critA':>8}{'critB':>8}{'A/4':>8}{'excess':>8}")
tot = collections.Counter()
for k in sorted(set(ra) | set(rb), key=lambda x: (-1 if x == 'gap' else x)):
    a, b = ra[k], rb[k]
    if max(a, b) < 0.5: continue
    tot['a'] += a; tot['b'] += b
    print(f"{('P'+str(k)) if k!='gap' else 'gap':6}{lab.get(k,'')[:55]:55}{a:8.1f}{b:8.1f}{a/4:8.1f}{b-a/4:8.1f}")
print(f"TOTAL crit A={tot['a']:.0f} B={tot['b']:.0f} excess={tot['b']-tot['a']/4:.0f}")
