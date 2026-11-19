import json,glob,os,collections,statistics as st
P='/opt/dlami/nvme/dfperf/runs/profile/'
tv=json.load(open(P+'quent/tip_vs_off.json'))
def nsys(q):
    its=[]
    for f in (P+f'nsys_out/q{q}.json',P+f'nsys_out/basetip_q{q}.json'):
        if os.path.exists(f): its+=[(os.path.basename(f),it) for it in json.load(open(f))]
    return its
def decomp(it):
    rg=it['ranges']; g=lambda n,k='wall': rg.get('dynfilter::'+n,{}).get(k,0)
    tail=0; ins_crit=0; hash_ins=0
    for s in it['sites']:
        w=max(0,s.get('contrib_kernels_end_after_contribute_return_ms') or 0)
        if s['publishing'] and 'accum::finish' in s['names']: tail+=min(w,s['finish_wall']) if s.get('pre_finish_ms',0)<1 else 0
    grp=collections.defaultdict(list)
    for s in it['sites']: grp[s['task']].append(sum(s.get('contribute_kern',{}).values()))
    ins_crit=sum(max(v) for v in grp.values())
    ins=sum(v for n in rg for k,v in rg[n]['kern'].items() if 'add_if' in k)
    return dict(nsys_wall=it['wall'],ins_crit=ins_crit,df=it['df_top_wall'],begin=g('construction::begin'),contrib=g('accum::contribute')+g('stage::contribute')+g('bounds::contribute'),
                finish=g('accum::finish'),gather=g('stage::gather'),repl=g('replicate_devices'),bounds=g('bounds::finish')+g('bounds::contribute'),late=g('accum::late_wait'),
                ins=ins,tail=tail,host=g('accum::finish')-tail+g('repl' if False else 'replicate_devices'),
                nsync=g('accum::finish','n_sync')+g('replicate_devices','n_sync')+g('stage::gather','n_sync'),
                mc_mb=sum(v[1] for n in ('accum::finish','replicate_devices') for v in rg.get('dynfilter::'+n,{}).get('mc',{}).values())/2**20,
                memset_mb=rg.get('dynfilter::construction::begin',{}).get('memset',[0,0,0])[1]/2**20)
rows={}
for q in sorted(tv,key=int):
    its=nsys(q)
    site=sum(r['delta'] for r in tv[q]['rows'] if r['role']=='DF site'); probe=sum(r['delta'] for r in tv[q]['rows'] if r['role']!='DF site')
    r=dict(q=int(q),wall=tv[q]['wall_tip'],off=tv[q]['wall_off'],site=site,probe=probe,ncap=len(its))
    if its:
        D=[decomp(it) for _,it in its]
        for k in D[0]: r[k]=st.mean(d[k] for d in D); r[k+'_rng']=(min(d[k] for d in D),max(d[k] for d in D))
        r['nonhost']=site-r['host']
    rows[q]=r
json.dump(rows,open(P+'combined.json','w'),indent=1)
print('| q | tip wall ms (Quent, 6 hot) | DF-off wall | DF-site delta on chain ms (tip-off) | % of q | nsys captures (hot iters) | begin ms | contribute host ms | insert kernel GPU ms (sum over GPUs) | insert kernel critical-path ms (max task per site) | accum::finish ms | stage::gather ms | replicate_devices ms | finish-chain syncs | HOST-staged (excluded) ms [min-max] | HOST-staged copy MB | DF-site delta minus HOST-staged ms | % of q | probe-scan delta ms (tip-off) |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
T=collections.Counter()
for q,r in rows.items():
    T['wall']+=r['wall']; T['site']+=r['site']; T['probe']+=r['probe']
    if r['ncap']:
        T['host']+=r['host']; T['nonhost']+=r['nonhost']; T['ins']+=r['ins']; T['ins_crit']+=r['ins_crit']
        print(f"| {q} | {r['wall']:.0f} | {r['off']:.0f} | {r['site']:+.1f} | {100*r['site']/r['wall']:.1f} | {r['ncap']} | {r['begin']:.2f} | {r['contrib']:.2f} | {r['ins']:.2f} | {r['ins_crit']:.2f} | {r['finish']:.1f} | {r['gather']:.2f} | {r['repl']:.2f} | {r['nsync']:.0f} | {r['host']:.1f} [{r['host_rng'][0]:.1f}-{r['host_rng'][1]:.1f}] | {r['mc_mb']:.0f} | {r['nonhost']:+.1f} | {100*r['nonhost']/r['wall']:.2f} | {r['probe']:+.1f} |")
    else:
        print(f"| {q} | {r['wall']:.0f} | {r['off']:.0f} | {r['site']:+.1f} | {100*r['site']/r['wall']:.1f} | 0 | | | | | | | | | not captured | | | | {r['probe']:+.1f} |")
print(dict(T))
