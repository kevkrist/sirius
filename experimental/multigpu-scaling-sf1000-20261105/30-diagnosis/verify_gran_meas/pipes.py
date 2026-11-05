import csv,sys,collections
qdir,qnum=sys.argv[1],sys.argv[2]
lab={r['pipeline']:r['label'] for r in csv.DictReader(open(qdir+'/pipelines.csv'))}
qs={r['query']:r for r in csv.DictReader(open(qdir+'/queries.csv'))}
T=collections.defaultdict(list)
for r in csv.DictReader(open(qdir+'/tasks.csv')):
    if r['query'].startswith(f'tpch_q{qnum}_iter'): T[r['query']].append(r)
for q in sorted(T):
    if q.endswith('iter1'): continue
    ts=T[q]; t0=min(int(r['created']) for r in ts)
    byp=collections.defaultdict(list)
    for r in ts: byp[r['pipeline']].append(r)
    print('==',q, qs.get(q,{}).get('wall_ms',''))
    for p,rs in sorted(byp.items(),key=lambda kv:min(int(r['preparing'] or 0) for r in kv[1])):
        st=min(int(r['preparing']) for r in rs if r['preparing']); en=max(int(r['finalize']) for r in rs if r['finalize'])
        g=collections.Counter(r['gpu'] for r in rs)
        gend={gg:max(int(r['finalize']) for r in rs if r['gpu']==gg and r['finalize']) for gg in g}
        ends=sorted((v-t0)/1e6 for v in gend.values())
        print(f"{len(rs):4d} {dict(sorted(g.items()))} span {(st-t0)/1e6:7.1f}-{(en-t0)/1e6:7.1f} gpu_ends {['%.1f'%e for e in ends]} {lab.get(p,p)[:70]}")
