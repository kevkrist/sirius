import json,statistics as st,re,difflib,collections
R='/opt/dlami/nvme/dfperf/runs/profile/quent/'
t=json.load(open(R+'critical_chain.json')); o=json.load(open(R+'critical_chain_off.json'))
dfj=json.load(open(R+'baseline_tip.json'))
sites=collections.defaultdict(dict)
for r in dfj:
    for j in r['joins']:
        if j.get('site'): sites[str(r['q'])][j['site']]=j
def agg(rs):
    d={}
    for r in rs:
        x={}
        for c in r['chain']: x[c['pipe']]=x.get(c['pipe'],0)+c['end']-c['created']
        for k,v in x.items(): d.setdefault(k,[]).append(v)
    return {k:(st.median(v),min(v),max(v)) for k,v in d.items()}, st.median([r['wall'] for r in rs])
sig=lambda s: re.sub(r'\(\d+\)','',s).replace(' -> DYNAMIC_FILTER','')
out={}
print('| q | pipeline (tip) | role | tip ms on chain (min-max) | DF-off ms | delta ms |'); print('|---|---|---|---|---|---|')
tot=collections.Counter()
for q in sorted(sites,key=int):
    a,wa=agg(t[q]); b,wb=agg(o[q]); ka=list(a); kb=list(b)
    sm=difflib.SequenceMatcher(None,[sig(x) for x in ka],[sig(x) for x in kb],autojunk=False)
    mp={}
    for tag,i1,i2,j1,j2 in sm.get_opcodes():
        if tag=='equal':
            for k in range(i2-i1): mp[ka[i1+k]]=kb[j1+k]
    out[q]=dict(wall_tip=wa,wall_off=wb,rows=[])
    for x in ka:
        role='DF site' if x in sites[q] else ('probe scan+DF' if 'DYNAMIC_FILTER' in x else None)
        if not role: continue
        y=mp.get(x); bo=b[y][0] if y else None
        d=a[x][0]-(bo or 0)
        out[q]['rows'].append(dict(pipe=x,role=role,tip=a[x][0],off=bo,delta=d,matched=y))
        tot[role]+=d
        if abs(d)>=1 or role=='DF site': print(f"| {q} | {x[:40]} | {role} | {a[x][0]:.1f} ({a[x][1]:.1f}-{a[x][2]:.1f}) | {'%.1f'%bo if bo is not None else 'no match'} | {d:+.1f} |")
print(dict(tot))
json.dump(out,open(R+'tip_vs_off.json','w'),indent=1)
