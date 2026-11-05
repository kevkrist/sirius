import json,glob,sys,collections,os
root=sys.argv[1]
def load(kind):
    out=[]
    for f in glob.glob(os.path.join(root,kind,'*.ndjson')):
        for l in open(f): out.append(json.loads(l))
    return out
ops={e['id']:e['data']['Declaration'] for e in load('Operator') if 'Declaration' in e['data']}
plans={e['id']:e['data']['Declaration']['query_id']['target'] for e in load('Plan') if 'Declaration' in e['data']}
queries={}; qtimes=collections.defaultdict(dict)
for e in load('Query'):
    k=list(e['data'])[0]
    if k=='Init': queries[e['id']]=e['data']['Init']['instance_name']
    qtimes[e['id']][k]=e['timestamp']
threads={}
for e in load('ExecutorThread'):
    if 'Spawned' in e['data']: threads[e['id']]=e['data']['Spawned']['label']
tasks=collections.defaultdict(dict)
for e in load('Task'):
    k=list(e['data'])[0]; tasks[e['id']][k]=(e['timestamp'],e['data'][k])
spaces={e['id']:e['data']['Declaration']['label'] for e in load('MemorySpace') if 'Declaration' in e['data']}
# per query per pipeline
agg=collections.defaultdict(lambda: collections.defaultdict(list))
for tid,t in tasks.items():
    if 'Created' not in t or 'Preparing' not in t or 'Computing' not in t: continue
    pid=t['Created'][1]['pipeline_uuid']['target']
    op=ops.get(pid,{}); q=queries.get(plans.get(op.get('plan_id',{}).get('target')),'?')
    prep=(t['Computing'][0]-t['Preparing'][0])/1e6
    end=t.get('Finalizing',(t['Computing'][0],))[0]
    comp=(end-t['Computing'][0])/1e6
    btm=t.get('Reserving',(0,{}))[1].get('bytes_to_materialize',0)
    th=threads.get(t['Preparing'][1]['executor_thread']['target'],'?')
    agg[q][op.get('label','?')].append((prep,comp,btm,t['Preparing'][1]['origin_tier'],th,t['Preparing'][1]['input_bytes'],t['Preparing'][0],end))
for q in sorted(agg):
    qt=qtimes
    print('=====',q)
    for lbl,v in agg[q].items():
        n=len(v); sp=sum(x[0] for x in v); sc=sum(x[1] for x in v)
        mx=max(v,key=lambda x:x[0])
        gpus=collections.Counter(x[4].split('-')[1] for x in v)
        t0=min(x[6] for x in v); t1=max(x[7] for x in v)
        print(f"{lbl[:70]:70s} n={n:4d} prep_sum={sp:8.1f}ms comp_sum={sc:8.1f}ms max_prep={mx[0]:7.1f}ms(btm={mx[2]/1e6:.1f}MB,{mx[3]}) span={(t1-t0)/1e6:7.1f}ms btm_sum={sum(x[2] for x in v)/1e9:.2f}GB gpus={dict(gpus)}")
for qid,name in queries.items():
    t=qtimes[qid]
    if 'Executing' in t and 'Finished' in t: print(name,(t['Finished']-t['Executing'])/1e6,'ms')
