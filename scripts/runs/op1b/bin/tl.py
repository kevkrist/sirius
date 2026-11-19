#!/usr/bin/env python3
"""tl.py <session-dir> <regex of pipeline labels> [iter]: per-task timeline (ms from query Executing) of matching pipelines."""
import sys,re,os
sys.path.insert(0,'/opt/dlami/nvme/dfperf/runs/profile/bin'); import qload
s=qload.load(sys.argv[1]); pat=re.compile(sys.argv[2]); want=int(sys.argv[3]) if len(sys.argv)>3 else None
for qi,(qid,qev) in enumerate(s['queries'].items()):
    if qi==0 or 'Finished' not in qev: continue
    if want is not None and qi!=want: continue
    t0=qev['Executing']; plans={p for p,q in s['plans'].items() if q==qid}
    rows=[]
    for tid,ev in s['tasks'].items():
        pid=ev[0][3]['pipeline_uuid']['target']
        if s['ops'][pid]['plan'] not in plans: continue
        lab=s['ops'][pid]['label']
        if not pat.search(lab): continue
        d={}
        for seq,t,k,v in ev: d.setdefault(k,t)
        last=max(t for seq,t,k,v in ev)
        rows.append((d.get('Created',0),lab,d,last))
    rows.sort()
    print(f"iter {qi} wall {(qev['Finished']-t0)/1e6:.1f} ms")
    for cr,lab,d,last in rows:
        f=lambda k: f"{(d[k]-t0)/1e6:8.1f}" if k in d else "       -"
        print(f"  {lab[:44]:44s} created {f('Created')} prep {f('Preparing')} comp {f('Computing')} fin {f('Finalizing')} last {(last-t0)/1e6:8.1f}")
