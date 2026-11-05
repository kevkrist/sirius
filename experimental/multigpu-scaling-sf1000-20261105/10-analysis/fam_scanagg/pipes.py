#!/usr/bin/env python3
"""Per-pipeline per-GPU timeline + op thread-time for a query label. usage: pipes.py <quentdir> <label> [<label>...]"""
import csv, sys, statistics as st
from collections import defaultdict
d=sys.argv[1]
labels=sys.argv[2:]
q={r['query']:r for r in csv.DictReader(open(d+'/queries.csv'))}
pl={r['pipeline']:r['label'] for r in csv.DictReader(open(d+'/pipelines.csv'))}
T=[r for r in csv.DictReader(open(d+'/tasks.csv')) if r['query'] in labels]
O=[r for r in csv.DictReader(open(d+'/task_ops.csv')) if r['query'] in labels]
for lab in labels:
    q0=int(q[lab]['init']); wall=float(q[lab]['wall_ms'])
    print(f"== {lab} wall {wall:.1f} ms")
    byp=defaultdict(list)
    for t in T:
        if t['query']==lab: byp[t['pipeline_type']].append(t)
    for p in sorted(byp,key=lambda x:int(x.split()[-1])):
        ts=byp[p]; lbl=pl.get(ts[0]['pipeline'],'')
        pg=defaultdict(list)
        for t in ts: pg[t['gpu']].append(t)
        print(f"  {p}: {lbl}  n={len(ts)}")
        for g in sorted(pg):
            x=pg[g]
            st0=min(int(t['preparing']) for t in x if t['preparing'])
            en=max(int(t['finalize']) for t in x if t['finalize'])
            prep=[(int(t['first_compute'])-int(t['preparing']))/1e6 for t in x if t['first_compute'] and t['preparing']]
            comp=[(int(t['finalize'])-int(t['first_compute']))/1e6 for t in x if t['first_compute'] and t['finalize']]
            orig=defaultdict(int)
            for t in x: orig[t['origin']]+=1
            print(f"    gpu{g}: n={len(x)} span {(st0-q0)/1e6:7.1f}..{(en-q0)/1e6:7.1f} ms  prep sum {sum(prep):7.1f} med {st.median(prep):6.2f}  comp sum {sum(comp):7.1f} med {st.median(comp):6.2f}  origin {dict(orig)}")
    ops=defaultdict(list)
    for o in O:
        if o['query']==lab: ops[o['operator_id']].append((int(o['end'])-int(o['start']))/1e6)
    print("  op thread-ms: "+"  ".join(f"op{k}: sum {sum(v):.0f} n {len(v)} med {st.median(v):.2f}" for k,v in sorted(ops.items(),key=lambda kv:int(kv[0]))))
