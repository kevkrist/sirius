import re,collections,csv
bloom={5:{16,22},7:{7},8:{25,31},9:{22}}
def tim(run):
    d=collections.defaultdict(list)
    for r in csv.DictReader(open(f'../../00-baseline/{run}/timings.csv')):
        if int(r['iter'])>1: d[int(r['query'])].append(float(r['seconds'])*1000)
    return {k:sum(v)/len(v) for k,v in d.items()}
T1,T4=tim('g1_a'),tim('g4_a')
cats=['LINK','BLOOM','FPART','CLONE','JOIN/AGG','GAP','OUTSIDE']
F=collections.Counter()
print(f"{'Q':3}"+''.join(f"{c:>10}" for c in cats)+f"{'sum':>8}{'gap(t)':>8}")
for q in [5,7,8,9]:
    C=collections.Counter(); crit1=crit4=0
    for l in open(f'crit_q{q}.txt'):
        m=re.match(r'(P\d+|gap)\s+(.*?)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)$',l.rstrip())
        if not m: continue
        p,lab,a,b,a4,ex=m.groups(); ex=float(ex); crit1+=float(a); crit4+=float(b)
        if p=='gap': c='GAP'
        else:
            pid=int(p[1:])
            if lab.startswith('GPU_SCAN'): c='LINK'
            elif pid in bloom[q]: c='BLOOM'
            elif lab.startswith('PARTITION'): c='FPART'
            elif lab.startswith('CONCAT'): c='CLONE'
            else: c='JOIN/AGG'
        C[c]+=ex
    out1=T1[q]-crit1; out4=T4[q]-crit4; C['OUTSIDE']=out4-out1/4
    gap=T4[q]-T1[q]/4
    for c in cats: F[c]+=C[c]
    F['gap']+=gap
    print(f"Q{q:<2}"+''.join(f"{C[c]:10.0f}" for c in cats)+f"{sum(C.values()):8.0f}{gap:8.0f}   (t1={T1[q]:.0f} t4={T4[q]:.0f} outside-quent g1={out1:.0f} g4={out4:.0f})")
print("FAM"+''.join(f"{F[c]:10.0f}" for c in cats)+f"{sum(F[c] for c in cats):8.0f}{F['gap']:8.0f}")
