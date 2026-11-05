import re,sys,collections
keys=['sirius::compression::host_to_gpu','sirius_physical_partition::execute','sirius_physical_dynamic_filter::execute','sirius_physical_table_scan::execute','sirius_physical_gpu_scan::execute','convert:gpu_to_gpu','convert:gpu_to_gpu:host_staging','convert:gpu_to_gpu:peer_copy','convert:host_to_gpu','sirius_physical_concat::execute','sirius_physical_hash_join::execute','sirius_physical_grouped_aggregate::execute','sirius_physical_projection::execute']
def parse(f):
    out={}; cur=None; taskconcat=[0,0,0.0]
    for l in open(f):
        m=re.match(r'== (.*): n=(\d+) wall=([\d.]+)ms launched_kernels=([\d.]+)ms',l)
        if m:
            cur=m.group(1); out[cur]={'n':int(m.group(2)),'wall':float(m.group(3)),'k':float(m.group(4)),'mem':{},'api':''}
            if re.match(r'Pipeline \d+ Task \d+ \[CONCAT',cur): taskconcat[0]+=1; taskconcat[1]+=float(m.group(3))
            continue
        m=re.match(r'\s+memcpy (\S+): ([\d.]+)ms ([\d.]+)GB',l)
        if m and cur: out[cur]['mem'][m.group(1)]=(float(m.group(2)),float(m.group(3)))
        m=re.match(r'\s+api: (.*)',l)
        if m and cur: out[cur]['api']=m.group(1)
    out['__concat_tasks']=taskconcat
    return out
for q in sys.argv[1:]:
    A=parse(f'nsys_q{q}_g1.txt'); B=parse(f'nsys_q{q}_g4.txt')
    print(f'### Q{q}  (thread-ms, nsys iter2)')
    print(f"{'range':45}{'n1':>6}{'n4':>6}{'wall1':>8}{'wall4':>8}{'kern1':>7}{'kern4':>7}  bytes1/4 GB")
    for k in keys:
        a=A.get(k,{'n':0,'wall':0,'k':0,'mem':{}}); b=B.get(k,{'n':0,'wall':0,'k':0,'mem':{}})
        ga=sum(v[1] for v in a['mem'].values()); gb=sum(v[1] for v in b['mem'].values())
        print(f"{k:45}{a['n']:6}{b['n']:6}{a['wall']:8.0f}{b['wall']:8.0f}{a['k']:7.0f}{b['k']:7.0f}  {ga:6.1f}/{gb:6.1f}")
    print('  CONCAT task ranges g1:',A['__concat_tasks'][:2],' g4:',B['__concat_tasks'][:2])
    for k in ['sirius_physical_partition::execute','convert:gpu_to_gpu:host_staging']:
        if k in B: print('  g4',k,'api:',B[k]['api'][:200])
