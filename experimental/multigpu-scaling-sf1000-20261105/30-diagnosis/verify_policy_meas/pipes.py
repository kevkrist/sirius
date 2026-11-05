import csv,collections,sys
B='/home/ubuntu/sirius-scaling-20261105/'
run,qq=sys.argv[1],sys.argv[2]
lab={r['pipeline']:r['label'] for r in csv.DictReader(open(B+run+'/quent/pipelines.csv'))}
qs={r['query']:int(r['init']) for r in csv.DictReader(open(B+run+'/quent/queries.csv'))}
P=collections.defaultdict(list)
for r in csv.DictReader(open(B+run+'/quent/tasks.csv')):
    q,it=r['query'].rsplit('_iter',1)
    if q!=qq or it not in('2','3','4') or not r['finalize']: continue
    t0=qs[r['query']]
    P[(r['pipeline_type'],lab.get(r['pipeline'],'')[:70])].append((it,(int(r['preparing'])-t0)/1e6,(int(r['finalize'])-t0)/1e6,(int(r['finalize'])-int(r['preparing']))/1e6, r['origin']))
for k,v in sorted(P.items(),key=lambda kv: min(x[1] for x in kv[1])):
    n=len(v)/3; s=min(x[1] for x in v if x[0]=='3'); e=max(x[2] for x in v if x[0]=='3')
    print('%-16s n=%5.0f start %7.1f end %7.1f thr_ms %8.1f  %s'%(k[0],n,s,e,sum(x[3] for x in v)/3,k[1]))
