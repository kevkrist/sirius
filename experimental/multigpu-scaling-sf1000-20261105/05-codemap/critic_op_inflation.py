# usage: python3 -I <this> <dir containing qf/g1 and qf/g4 from bin/quent_flatten.py>
import csv,collections,sys,re
S=sys.argv[1]; qs=sys.argv[2].split(',')
def opnames(run):
    m={}
    for r in csv.DictReader(open(f'{S}/qf/{run}/pipelines.csv')):
        for name,oid in re.findall(r'([A-Z_]+)\((\d+)\)', r['label']):
            m[(r['query'],oid)]=name
    return m
def load(run):
    names=opnames(run); agg=collections.defaultdict(lambda: collections.Counter()); cnt=collections.defaultdict(collections.Counter); byt=collections.defaultdict(collections.Counter)
    for r in csv.DictReader(open(f'{S}/qf/{run}/task_ops.csv')):
        q=r['query']
        if not q.endswith('iter4'): continue
        key=names.get((q,r['operator_id']),'?')+'('+r['operator_id']+')'
        agg[q][key]+=(int(r['end'])-int(r['start']))/1e6; cnt[q][key]+=1; byt[q][key]+=int(r['input_bytes'] or 0)
    return agg,cnt,byt
A1,C1,B1=load('g1'); A4,C4,B4=load('g4')
tot1=collections.Counter(); tot4=collections.Counter()
for q in A1:
    for k,v in A1[q].items(): tot1[k.split('(')[0]]+=v
for q in A4:
    for k,v in A4[q].items(): tot4[k.split('(')[0]]+=v
print('ALL-QUERIES operator-type compute ms (iter4): type g1 g4 ratio')
for k in sorted(set(tot1)|set(tot4), key=lambda k:-(tot4[k]-tot1[k])):
    print(f'  {k:28s}{tot1[k]:9.0f}{tot4[k]:9.0f}{tot4[k]/max(tot1[k],1):7.2f}  delta={tot4[k]-tot1[k]:8.0f}')
for qn in qs:
    q=f'tpch_q{qn}_iter4'; print('==',q)
    for k in sorted(set(A1[q])|set(A4[q]), key=lambda k:-(A4[q][k])):
        if A4[q][k]+A1[q][k]<50: continue
        print(f'  {k:28s} n={C1[q][k]:5d}/{C4[q][k]:5d} ms={A1[q][k]:8.0f}/{A4[q][k]:8.0f} x{A4[q][k]/max(A1[q][k],1):5.2f}  GB={B1[q][k]/1e9:7.1f}/{B4[q][k]/1e9:7.1f}')
