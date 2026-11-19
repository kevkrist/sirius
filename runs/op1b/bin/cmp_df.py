#!/usr/bin/env python3
"""cmp_df.py <tip df_summary.json> <proto df_summary.json>: filter coverage and consumer row counts, hot iteration of a TRACE pass."""
import json,sys
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2]))
same=0; diff=0; tk=pk=0
for q in sorted(set(a)|set(b),key=int):
    if q not in a or q not in b: print(f"q{q}: missing in {'tip' if q not in a else 'proto'}"); diff+=1; continue
    x,y=a[q],b[q]; d=[]
    kx=sorted((k['join'],k['condition'],k['kind'],k['size'],k['pushed'],k['reason']) for j in x['joins'] for k in j['keys'])
    ky=sorted((k['join'],k['condition'],k['kind'],k['size'],k['pushed'],k['reason']) for j in y['joins'] for k in j['keys'])
    tk+=sum(int(k[4]) for k in kx); pk+=sum(int(k[4]) for k in ky)
    if kx!=ky: d.append(f"keys differ: tip-only {sorted(set(kx)-set(ky))} proto-only {sorted(set(ky)-set(kx))}")
    for f in ('constructions','bloom_bytes','host'):
        if x.get(f)!=y.get(f): d.append(f"{f}: {x.get(f)} -> {y.get(f)}")
    if x.get('kept')!=y.get('kept'): d.append(f"cscan kept (probes ran/of/complete -> [chunks, kept, total]): {x.get('kept')} -> {y.get('kept')}")
    ox={k:(v['rows_in'],v['rows_out']) for k,v in x.get('df_ops',{}).items()}; oy={k:(v['rows_in'],v['rows_out']) for k,v in y.get('df_ops',{}).items()}
    if ox!=oy: d.append("DYNAMIC_FILTER rows in/out: "+"; ".join(f"op{k}: {ox.get(k)} -> {oy.get(k)}" for k in sorted(set(ox)|set(oy),key=int) if ox.get(k)!=oy.get(k)))
    if x.get('errors')!=y.get('errors'): d.append(f"errors {x.get('errors')} -> {y.get('errors')}")
    if d: diff+=1; print(f"q{q}: DIFF\n   "+"\n   ".join(d))
    else: same+=1; print(f"q{q}: same ({len(kx)} keys, kept {x.get('kept')})")
print(f"{same} same, {diff} differ; filters pushed tip {tk} proto {pk}")
