# usage: python3 -I <this> <dir containing qf/g1 and qf/g4 from bin/quent_flatten.py>
import csv,collections,sys
S=sys.argv[1]
def load(run):
    Q={r['query']:float(r['wall_ms']) for r in csv.DictReader(open(f'{S}/qf/{run}/queries.csv'))}
    agg=collections.defaultdict(lambda: collections.Counter())
    for r in csv.DictReader(open(f'{S}/qf/{run}/tasks.csv')):
        q=r['query']
        if not q.endswith('iter4'): continue
        try:
            p=int(r['preparing']);f=int(r['first_compute']);e=int(r['finalize'])
        except: continue
        a=agg[q]; a['n']+=1
        a['prep_'+r['origin']]+=(f-p)/1e6; a['comp']+=(e-f)/1e6
        a['bytes_'+r['origin']]+=int(r['in_bytes'] or 0)
    return Q,agg
Q1,A1=load('g1'); Q4,A4=load('g4')
print(f"{'q':6}{'wall1':>8}{'wall4':>8}{'x':>6} | {'prepH1':>8}{'prepH4':>8} {'prepG1':>8}{'prepG4':>8} | {'comp1':>8}{'comp4':>8}{'cInfl':>6} | {'tot1':>8}{'tot4':>8}{'infl':>6} | {'occ1':>5}{'occ4':>5} | {'n1':>6}{'n4':>6}")
T=collections.Counter()
for i in range(1,23):
    q=f'tpch_q{i}_iter4'; a1=A1[q]; a4=A4[q]; w1=Q1[q]; w4=Q4[q]
    t1=a1['prep_HOST']+a1['prep_GPU']+a1['comp']; t4=a4['prep_HOST']+a4['prep_GPU']+a4['comp']
    for k,v in [('w1',w1),('w4',w4),('pH1',a1['prep_HOST']),('pH4',a4['prep_HOST']),('pG1',a1['prep_GPU']),('pG4',a4['prep_GPU']),('c1',a1['comp']),('c4',a4['comp']),('t1',t1),('t4',t4)]: T[k]+=v
    print(f"{i:<6}{w1:8.0f}{w4:8.0f}{w1/w4:6.2f} | {a1['prep_HOST']:8.0f}{a4['prep_HOST']:8.0f} {a1['prep_GPU']:8.0f}{a4['prep_GPU']:8.0f} | {a1['comp']:8.0f}{a4['comp']:8.0f}{a4['comp']/max(a1['comp'],1):6.2f} | {t1:8.0f}{t4:8.0f}{t4/t1:6.2f} | {t1/w1/4:5.2f}{t4/w4/16:5.2f} | {a1['n']:6}{a4['n']:6}")
print('TOT', ' '.join(f'{k}={v:.0f}' for k,v in T.items()), 'prepH infl',T['pH4']/T['pH1'],'prepG infl',T['pG4']/max(T['pG1'],1),'comp infl',T['c4']/T['c1'],'tot infl',T['t4']/T['t1'])
print('other origins g4:', {k for a in A4.values() for k in a if k.startswith('prep_')})
