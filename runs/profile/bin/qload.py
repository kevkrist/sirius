import json,glob,collections,os
def load(sess):
    def rd(d):
        for f in glob.glob(os.path.join(sess,d,'*.ndjson')):
            for l in open(f):
                e=json.loads(l); k=next(iter(e['data'])); yield e['id'],e['timestamp'],k,e['data'][k]
    queries=collections.OrderedDict()
    for i,t,k,v in sorted(rd('Query'),key=lambda x:x[1]):
        queries.setdefault(i,{})[k]=t
    plans={}; edges={}
    for i,t,k,v in rd('Plan'):
        plans[i]=v['query_id']['target']; edges[i]=[(e['source']['target'],e['target']['target']) for e in v['edges']]
    ops={}
    for i,t,k,v in rd('Operator'):
        ops[i]=dict(label=v['label'],pipe=v['type_name'],plan=v['plan_id']['target'])
    ports={}
    for i,t,k,v in rd('Port'):
        ports[i]=(v['operator_id']['target'],v['label'])
    tasks=collections.defaultdict(list)
    for i,t,k,v in rd('Task'):
        tasks[i].append((v.get('seq',0),t,k,v))
    for i in tasks: tasks[i].sort(key=lambda x:x[0])
    batches=collections.defaultdict(dict)
    for i,t,k,v in rd('DataBatch'):
        b=batches[i]; b[k]=t
        if k=='Constructed': b['prod']=v['producer_pipeline_id']['target']; b['bid']=v['data_batch_id']
        if k=='Stationary': b.setdefault('bytes',v['memory']['data']['bytes']); b.setdefault('mem',v['memory']['target'])
    return dict(queries=queries,plans=plans,edges=edges,ops=ops,ports=ports,tasks=tasks,batches=batches)
