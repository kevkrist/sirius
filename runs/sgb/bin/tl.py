#!/usr/bin/env python3
# tl.py <session_dir> [hot_index=1] [minms=1.0] : task timeline of one hot query (Quent)
import sys,os,json,collections
sys.path.insert(0,'/opt/dlami/nvme/dfperf/runs/profile/bin'); import qload
s=qload.load(sys.argv[1]); hi=int(sys.argv[2]) if len(sys.argv)>2 else 1; minms=float(sys.argv[3]) if len(sys.argv)>3 else 1.0
qs=list(s['queries'].items()); qid,qev=qs[hi]; t0=qev['Executing']; print('wall ms',(qev['Finished']-t0)/1e6)
plans=[p for p,q in s['plans'].items() if q==qid]; ops={i:o for i,o in s['ops'].items() if o['plan'] in plans}
rows=[]
for tid,ev in s['tasks'].items():
    pid=ev[0][3]['pipeline_uuid']['target']
    if pid not in ops: continue
    d={}; keys=set()
    for seq,t,k,v in ev:
        d.setdefault(k,t); keys.add(k)
        if k=='Preparing': d['in']=v.get('input_bytes'); d['tier']=v.get('origin_tier'); d['v']=v
        if k=='Computing': d['cv']=v
        last=t
    d['end']=last; d['pipe']=ops[pid]['pipe'] if 'pipe' in ops[pid] else ''; d['label']=ops[pid]['label']; d['ev']=ev
    rows.append(d)
rows.sort(key=lambda d:d.get('Created',0))
for d in rows:
    c=d.get('Created',0); p=d.get('Preparing'); co=d.get('Computing'); e=d['end']
    if (e-c)/1e6<minms: continue
    f=lambda x: '%8.1f'%((x-t0)/1e6) if x else '       -'
    extra=''
    if len(sys.argv)>4:
        extra=' '.join('%s@%.1f'%(k,(t-t0)/1e6) for _,t,k,_ in d['ev'])
    print(f(c),f(p),f(co),f(e),'in=%7.1fMB'%((d.get('in') or 0)/1e6),str(d.get('tier')),d['label'][:70],extra)
