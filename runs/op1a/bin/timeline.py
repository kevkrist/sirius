#!/usr/bin/env python3
# timeline.py <session_dir> [iter=2] [t_lo t_hi] : per-pipeline task timeline of one iteration (ms since Executing)
import sys,os,collections,re
sys.path.insert(0,'/opt/dlami/nvme/dfperf/runs/profile/bin'); import qload
sess=sys.argv[1]; it=int(sys.argv[2]) if len(sys.argv)>2 else 2
lo=float(sys.argv[3]) if len(sys.argv)>3 else -1; hi=float(sys.argv[4]) if len(sys.argv)>4 else 1e12
s=qload.load(sess); qs=list(s['queries'].items()); qid,qev=qs[it]; t0=qev['Executing']
print('wall %.1f ms'%((qev['Finished']-t0)/1e6))
plans=[p for p,q in s['plans'].items() if q==qid]
ops={i:o for i,o in s['ops'].items() if o['plan'] in plans}
rows=[]
for tid,ev in s['tasks'].items():
    pid=ev[0][3]['pipeline_uuid']['target']
    if pid not in ops: continue
    d={}
    for seq,t,k,v in ev:
        if k=='Created': d['cr']=t
        elif k=='Preparing': d.setdefault('pr',t)
        elif k=='Computing': d.setdefault('c0',t); d['dev']=v.get('reservation',{}).get('target')
        elif k=='Finalizing': d['end']=t
        elif k=='Routing': d['gpu']=v.get('preferred_device_id')
    if 'end' in d: rows.append((pid,d))
ms=lambda t:(t-t0)/1e6
byp=collections.defaultdict(list)
for pid,d in rows: byp[pid].append(d)
out=[]
for pid,ds in byp.items():
    cr=min(d['cr'] for d in ds); en=max(d['end'] for d in ds)
    out.append((cr,pid,ds,en))
for cr,pid,ds,en in sorted(out):
    if ms(en)<lo or ms(cr)>hi: continue
    lab=ops[pid]['label']
    print('%8.1f -> %8.1f  n=%3d  %s'%(ms(cr),ms(en),len(ds),lab[:110]))
    if len(sys.argv)>5:
        for d in sorted(ds,key=lambda d:d['cr']):
            print('      cr %8.1f pr %8.1f c0 %8.1f end %8.1f gpu %s'%(ms(d['cr']),ms(d.get('pr',d['cr'])),ms(d.get('c0',d['cr'])),ms(d['end']),d.get('gpu')))
