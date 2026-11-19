import json,sys,collections
R='/opt/dlami/nvme/dfperf/runs/profile/nsys_out/'
QS=[21,3,7,9,10,8,5,2,4,12]
MB=2**20
def mean(a): return sum(a)/len(a)
print('### Per-query totals (mean of 2 hot iterations; [iter1, iter2])\n')
print('| q | nsys wall ms | DF ranges wall ms (top-level sum) | % wall | DF-site tasks: n, task wall ms | DF wall inside those tasks ms | all partition::execute ms (n) | DF % of all partition::execute | insert kernels GPU ms (add_if_n) | finish-chain wall ms | of which HOST-staged loop (excl.) ms | DF wall minus HOST-staged ms | % wall |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
allr={}
for q in QS:
    d=json.load(open(f'{R}q{q}.json')); allr[q]=d
    row=[]
    for it in d:
        rg=it['ranges']; sites=it['sites']
        fin=sum(rg[n]['wall'] for n in rg if n in('dynfilter::accum::finish','dynfilter::stage::gather','dynfilter::bounds::finish','dynfilter::replicate_devices'))
        # HOST-staged loop wall = accum::finish wall minus wait for insert kernels still running at finish start (approx: per site)
        host=0
        for s in sites:
            if s['publishing'] and 'accum::finish' in s['names']:
                w=s.get('contrib_kernels_end_after_contribute_return_ms') or 0
                host+=max(0,s['finish_wall']-max(0,w))
        # bloom finish without sources/targets? keep
        ins=sum(v for n in rg for k,v in rg[n]['kern'].items() if 'add_if' in k or 'bloom_filter' in k) if rg else 0
        tw=sum(s['task_wall'] or s['wall'] for s in sites)
        pe_df=sum(s['df_wall'] for s in sites if s['op']=='partition')
        row.append(dict(wall=it['wall'],df=it['df_top_wall'],nt=len(sites),tw=tw,dfin=sum(s['df_wall'] for s in sites),pe=it['partition_execute_all_ms'],pen=it['partition_execute_n'],pe_df=pe_df,ins=ins,fin=fin,host=host))
    m=lambda k: mean([r[k] for r in row]); b=lambda k: '[%s]'%', '.join('%.1f'%r[k] for r in row)
    print(f"| {q} | {m('wall'):.0f} {b('wall')} | {m('df'):.1f} {b('df')} | {100*m('df')/m('wall'):.1f} | {m('nt'):.0f}, {m('tw'):.1f} | {m('dfin'):.1f} | {m('pe'):.1f} ({m('pen'):.0f}) | {100*m('pe_df')/m('pe'):.0f} | {m('ins'):.2f} | {m('fin'):.1f} | {m('host'):.1f} | {m('df')-m('host'):.1f} | {100*(m('df')-m('host'))/m('wall'):.2f} |")
print('\n### Range sums per hot iteration (mean of 2)\n')
print('| q | range | n | wall ms | max ms | CUDA API calls (per range) | sync calls / ms | kernels: name n GPU-ms | memset MB | memcpy MB by kind (GPU copy ms) | top APIs |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
for q in QS:
    d=allr[q]; names=sorted(set(n for it in d for n in it['ranges']))
    for n in names:
        L=[it['ranges'][n] for it in d if n in it['ranges']]; k=len(d)
        f=lambda key: sum(x[key] for x in L)/k
        kern=collections.Counter(); kn=collections.Counter(); mc=collections.defaultdict(lambda:[0,0]); api=collections.Counter()
        for x in L:
            for a,v in x['kern'].items(): kern[a.split('::')[-1] or a]+=v/k
            for a,v in x['kern_n'].items(): kn[a.split('::')[-1] or a]+=v/k
            for a,v in x['mc'].items(): mc[a][0]+=v[1]/k; mc[a][1]+=v[2]/k
            for a,v in x['api'].items(): api[a]+=v/k
        ks='; '.join(f"{a} {kn[a]:.0f} {v:.2f}" for a,v in kern.most_common(4)) or '-'
        ms='; '.join(f"{a} {v[0]/MB:.0f} ({v[1]:.1f})" for a,v in sorted(mc.items())) or '-'
        ap=', '.join(f"{a.replace('cuda','')} {v:.0f}" for a,v in api.most_common(5))
        print(f"| {q} | {n.replace('dynfilter::','')} | {f('n'):.0f} | {f('wall'):.2f} | {max(x['max_wall'] for x in L):.2f} | {f('n_api'):.0f} ({f('n_api')/max(f('n'),1):.1f}) | {f('n_sync'):.0f} / {f('sync_ms'):.2f} | {ks} | {sum(x['memset'][1] for x in L)/k/MB:.0f} | {ms} | {ap} |")
print('\n### DF-site tasks (iteration 2 of each capture; tasks with task wall >= 1 ms or publishing)\n')
print('| q | task | op | publishing | task wall ms | execute range ms | DF ranges ms | contribute ranges ms (API calls) | insert kernel GPU ms | insert kernels end after contribute returns ms | pre-finish ms (hash path: scatter) | finish chain ms | syncs (ms) in task | HOST/Peer copy MB | post-finish ms |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for q in QS:
    it=allr[q][-1]
    for s in sorted(it['sites'],key=lambda s:s['start']):
        tw=s['task_wall'] or s['wall']
        if tw<1 and not s['publishing']: continue
        ck=sum(s.get('contribute_kern',{}).values())
        mc='; '.join(f"{a} {v[1]/MB:.0f}" for a,v in sorted(s['mc'].items())) or '-'
        g=lambda k: ('%.2f'%s[k]) if s.get(k) is not None else '-'
        print(f"| {q} | {(s['task'] or '?')[:34]} @{s['start']:.0f} | {s['op']} | {'Y' if s['publishing'] else ''} | {tw:.2f} | {s['wall']:.2f} | {s['df_wall']:.2f} | {s['contribute_wall']:.2f} ({s.get('contribute_n_api','-')}) | {ck:.2f} | {g('contrib_kernels_end_after_contribute_return_ms')} | {g('pre_finish_ms')} | {s['finish_wall']:.2f} | {s['n_sync']} ({s['sync_ms']:.1f}) | {mc} | {g('post_finish_ms')} |")
