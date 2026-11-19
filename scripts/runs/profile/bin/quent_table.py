import json,sys,statistics as st,collections
R='/opt/dlami/nvme/dfperf/runs/profile/quent/'
d=json.load(open(R+'baseline_tip.json'))+json.load(open(R+'baseline_tip_b.json'))
med=st.median
by=collections.defaultdict(list)
for r in d: by[r['q']].append(r)
print('| q | wall | join | site | path | kinds | bloom MB | tasks | med ms | last ms | sum ms | union ms | resv max ms | pub @ms | consumer scan(s): first prep - pub ms / tasks before pub / out bytes before pub |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
tot={}
for q in sorted(by):
    rs=by[q]; wall=med([r['wall_ms'] for r in rs]); nj=len(rs[0]['joins'])
    tot[q]=dict(wall=wall,union=med([r['build_union_all'] for r in rs]),states=[r['other_task_states'] for r in rs if r['other_task_states']])
    for ji in range(nj):
        js=[r['joins'][ji] for r in rs]; j=js[0]
        if 'n_tasks' not in j: print(f"| {q} | {wall:.0f} | {j['join']} | {j.get('site')} | {j.get('err')} |"); continue
        m=lambda k: med([x[k] for x in js])
        cons=[]
        for ci in range(len(j.get('consumers',[]))):
            cs=[x['consumers'][ci] for x in js if len(x.get('consumers',[]))>ci]
            c=cs[0]
            cons.append(f"{c['scan'].split(' ->')[0]}{'' if c['primary'] else '*'}: {med([x['gap_pub_to_first'] for x in cs]):+.1f} / {max(x['tasks_started_before_pub'] for x in cs)} of {c['n_tasks']} / {max(x['out_bytes_before'] for x in cs)/1e6:.0f}MB")
        path='hash' if (j['layout'].endswith('hash') and not j['broadcast']) else 'fwd'
        print(f"| {q} | {wall:.0f} | {j['join']} | {j['site']} | {path} | {j['kinds']} | {j['bloom_bytes']/1e6:.0f} | {m('n_tasks'):.0f} | {m('comp_med'):.2f} | {m('comp_last'):.2f} | {m('comp_sum'):.1f} | {m('build_union'):.1f} | {max(x['resv_wait_max'] for x in js):.2f} | {m('tpub'):.0f} | {'; '.join(cons)} |")
print()
print('| q | wall ms | union of DF-site task wall ms | % |'); print('|---|---|---|---|')
tw=tu=0
for q,t in tot.items():
    print(f"| {q} | {t['wall']:.0f} | {t['union']:.1f} | {100*t['union']/t['wall']:.1f} | {t['states'] or ''}"); tw+=t['wall']; tu+=t['union']
print('sum',tw,tu)
