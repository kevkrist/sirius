import csv,re,sys
from collections import defaultdict
B='/home/ubuntu/sirius-scaling-20261105/'
def load(d,it='2'):
    pl={}
    for r in csv.DictReader(open(B+d+'/quent/pipelines.csv')):
        m={}
        for name,i in re.findall(r'([A-Z_]+)\((\d+)\)',r['label']): m[i]=name
        pl[r['pipeline']]=m
    ops=defaultdict(float); n=defaultdict(int)
    for r in csv.DictReader(open(B+d+'/quent/task_ops.csv')):
        if not r['query'].endswith('_iter'+it): continue
        if r['start']=='' or r['end']=='': continue
        q=int(r['query'].split('_')[1][1:])
        # need pipeline: get from tasks
        ops[(q,r['task'],r['operator_id'])]+= (int(r['end'])-int(r['start']))/1e9
    tp={}; prep=defaultdict(float)
    for r in csv.DictReader(open(B+d+'/quent/tasks.csv')):
        if not r['query'].endswith('_iter'+it): continue
        tp[r['task']]=r['pipeline']
        q=int(r['query'].split('_')[1][1:])
        if r['preparing'] and r['first_compute']:
            prep[(q,r['origin'])]+=(int(r['first_compute'])-int(r['preparing']))/1e9
    agg=defaultdict(float)
    for (q,t,o),v in ops.items():
        name=pl.get(tp.get(t),{}).get(o,'?')
        agg[(q,name)]+=v
    return agg,prep
runs=sys.argv[1:]
data=[load(r) for r in runs]
names=sorted({k[1] for a,_ in data for k in a})
print('op thread-s per run:',runs)
for nm in names:
    print('%-16s'%nm,' '.join('%8.2f'%sum(v for k,v in a.items() if k[1]==nm) for a,_ in data))
for org in ['HOST','GPU']:
    print('prep %-11s'%org,' '.join('%8.2f'%sum(v for k,v in p.items() if k[1]==org) for _,p in data))
if len(sys.argv)>1:
  print('per query PARTITION / HASH_GROUP_BY:')
  for q in range(1,23):
    print(q,' '.join('%7.3f'%a.get((q,'PARTITION'),0) for a,_ in data),' | ',' '.join('%7.3f'%a.get((q,'HASH_GROUP_BY'),0) for a,_ in data))
