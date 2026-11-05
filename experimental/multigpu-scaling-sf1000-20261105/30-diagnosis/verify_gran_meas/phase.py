import csv,sys,collections,statistics as st
# usage: phase.py quentdir qnum substr...   -> per hot iter: phase span (first preparing -> last finalize) for pipelines whose label contains substr
qdir,qnum,subs=sys.argv[1],sys.argv[2],sys.argv[3:]
lab={r['pipeline']:r['label'] for r in csv.DictReader(open(qdir+'/pipelines.csv'))}
qs={r['query']:float(r['wall_ms']) for r in csv.DictReader(open(qdir+'/queries.csv'))}
T=collections.defaultdict(lambda:collections.defaultdict(list))
for r in csv.DictReader(open(qdir+'/tasks.csv')):
    if r['query'].startswith(f'tpch_q{qnum}_iter') and not r['query'].endswith('iter1'): T[r['query']][r['pipeline']].append(r)
res=collections.defaultdict(list)
for q in sorted(T):
    for p,rs in T[q].items():
        L=lab.get(p,'')
        for s in subs:
            if L.startswith(s):
                stt=min(int(r['preparing']) for r in rs); en=max(int(r['finalize']) for r in rs)
                res[s].append((stt and (en-stt)/1e6, len(rs)))
    res['wall'].append((qs.get(q,0),0))
for s,v in res.items(): print(f"{s:30s} mean {st.mean(x for x,_ in v):8.1f} ms  vals {[round(x,1) for x,_ in v]} ntasks {[n for _,n in v]}")
