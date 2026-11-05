import csv,collections,sys
def load(d):
    t=collections.defaultdict(list)
    for r in csv.DictReader(open(f'/home/ubuntu/sirius-scaling-20261105/{d}/timings.csv')):
        if int(r['iter'])>=2: t[int(r['query'])].append(float(r['seconds']))
    return {q:sum(v)/len(v) for q,v in t.items()}
g1=load('00-baseline/g1_a'); g4=load('00-baseline/g4_a'); jp=load('20-experiments/x_g4_joinpol'); g1jp=load('20-experiments/x_g1_joinpol')
b4=load('20-experiments/x_g4_batch4g'); be=load('20-experiments/x_g4_best'); g1b4=load('20-experiments/x_g1_batch4g'); g1be=load('20-experiments/x_g1_best')
print("q   g4    jp   dT4    dgap_eq | b4   best  dT4   dgap_eq")
S=collections.Counter()
for q in range(1,23):
    d1=jp[q]-g4[q]; e1=d1-(g1jp[q]-g1[q])/4
    d2=be[q]-b4[q]; e2=d2-(g1be[q]-g1b4[q])/4
    S['blanket_dT4']+=d1; S['oracle_dT4']+=min(0,d1); S['blanket_eq']+=e1; S['oracle_eq']+=min(0,e1) if d1<0 else 0
    S['b4_blanket_dT4']+=d2; S['b4_oracle_dT4']+=min(0,d2); S['b4_oracle_eq']+=min(0,e2) if d2<0 else 0
    print(f"{q:<3}{g4[q]:6.3f}{jp[q]:6.3f}{d1:+7.3f}{e1:+8.3f} |{b4[q]:6.3f}{be[q]:6.3f}{d2:+7.3f}{e2:+8.3f}")
for k,v in S.items(): print(k, round(v,3))
