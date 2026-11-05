# usage: python3 -I <this> <dir containing qf/g1 and qf/g4 from bin/quent_flatten.py>
import csv,collections,sys,bisect,statistics as st
S=sys.argv[1]
for run in ['g1','g4']:
  for q,opid in [('tpch_q1_iter4','4'),('tpch_q6_iter4','2'),('tpch_q6_iter4','0'),('tpch_q18_iter4','5')]:
    prep=[]  # HOST prepare intervals
    for r in csv.DictReader(open(f'{S}/qf/{run}/tasks.csv')):
        if r['query']!=q or r['origin']!='HOST': continue
        try: prep.append((int(r['preparing']),int(r['first_compute']),int(r['gpu'])))
        except: pass
    ops=[(int(r['start']),int(r['end']),int(r['gpu'])) for r in csv.DictReader(open(f'{S}/qf/{run}/task_ops.csv')) if r['query']==q and r['operator_id']==opid]
    buckets=collections.defaultdict(list); bsame=collections.defaultdict(list)
    for s,e,g in ops:
        m=(s+e)//2
        n=sum(1 for a,b,gg in prep if a<=m<b)
        ns=sum(1 for a,b,gg in prep if a<=m<b and gg//2==g//2)
        buckets[min(n,16)//2*2].append((e-s)/1e6)
    print(run,q,'op',opid,'n',len(ops),'median ms',round(st.median([(e-s)/1e6 for s,e,g in ops]),2),
          ' by #HOST-preparing tasks (all GPUs):',' '.join(f'{k}:{st.median(v):.2f}({len(v)})' for k,v in sorted(buckets.items())))
    pg=collections.defaultdict(list)
    for s,e,g in ops: pg[g].append((e-s)/1e6)
    print('    per-gpu median', {g:round(st.median(v),2) for g,v in sorted(pg.items())})
