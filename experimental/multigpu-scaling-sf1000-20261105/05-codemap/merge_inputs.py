import json,glob,sys,collections,os
root=sys.argv[1]
def load(kind):
    for f in glob.glob(os.path.join(root,kind,'*.ndjson')):
        for l in open(f): yield json.loads(l)
spaces={e['id']:e['data']['Declaration']['label'] for e in load('MemorySpace') if 'Declaration' in e['data']}
ops={e['id']:e['data']['Declaration']['label'] for e in load('Operator') if 'Declaration' in e['data']}
bid2space={}; bid2bytes={}
uuid2bid={}
for e in load('DataBatch'):
    d=e['data']
    if 'Constructed' in d: uuid2bid[e['id']]=d['Constructed']['data_batch_id']
    if 'Stationary' in d:
        b=uuid2bid.get(e['id']); bid2space[b]=spaces.get(d['Stationary']['memory']['target'],'?'); bid2bytes[b]=d['Stationary']['memory']['data']['bytes']
tasks=collections.defaultdict(dict)
for e in load('Task'):
    k=list(e['data'])[0]; tasks[e['id']].setdefault(k,[]).append((e['timestamp'],e['data'][k]))
for tid,t in tasks.items():
    if 'Created' not in t: continue
    lbl=ops.get(t['Created'][0][1]['pipeline_uuid']['target'],'')
    if not (lbl.startswith('MERGE')): continue
    comps=t.get('Computing',[])
    ids=comps[0][1]['input_batch_ids'] if comps else []
    c=collections.Counter(bid2space.get(i,'?')[:40] for i in ids)
    print(lbl, 'dev=',t['Routing'][0][1]['preferred_device_id'], 'n_inputs=',len(ids), dict(c), 'bytes=',sum(bid2bytes.get(i,0) for i in ids))
