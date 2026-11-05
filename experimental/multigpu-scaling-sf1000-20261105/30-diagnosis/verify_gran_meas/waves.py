import csv,sys,collections
qdir,qlab,sub=sys.argv[1:4]
lab={r['pipeline']:r['label'] for r in csv.DictReader(open(qdir+'/pipelines.csv'))}
rs=[r for r in csv.DictReader(open(qdir+'/tasks.csv')) if r['query']==qlab and lab.get(r['pipeline'],'').startswith(sub)]
t0=min(int(r['preparing']) for r in rs)
by=collections.defaultdict(list)
for r in rs: by[r['gpu']].append(((int(r['preparing'])-t0)/1e6,(int(r['finalize'])-t0)/1e6,r['thread'][-6:]))
for g in sorted(by):
    v=sorted(by[g]); print(g,len(v),' '.join(f"{a:.0f}-{b:.0f}" for a,b,_ in v))
# concurrency profile
ev=[]
for r in rs: ev+= [((int(r['preparing'])-t0)/1e6,1),((int(r['finalize'])-t0)/1e6,-1)]
ev.sort(); n=0; last=0; acc=collections.Counter()
for t,d in ev: acc[n]+=t-last; n+=d; last=t
print('time at concurrency', {k:round(v,1) for k,v in sorted(acc.items())})
