import csv,statistics,sys
B='/home/ubuntu/sirius-scaling-20261105/'
def load(d):
    r={}
    for row in csv.DictReader(open(B+d+'/timings.csv')):
        try: q=int(row['query'])
        except: continue
        it=int(row['iter'])
        if it>=2: r.setdefault(q,[]).append(float(row['seconds']))
    return r
R={k:load(v) for k,v in dict(g1='00-baseline/g1_a',g1b='00-baseline/g1_b',g4='00-baseline/g4_a',g4b='00-baseline/g4_b',
  jp4='20-experiments/x_g4_joinpol',jp1='20-experiments/x_g1_joinpol',b4='20-experiments/x_g4_batch4g',best4='20-experiments/x_g4_best',
  b1='20-experiments/x_g1_batch4g',best1='20-experiments/x_g1_best').items()}
m=lambda k,q: statistics.mean(R[k][q])
sd=lambda k,q: statistics.stdev(R[k][q])
Q=range(1,23)
tot=lambda k: sum(m(k,q) for q in Q)
print('totals',{k:round(tot(k),3) for k in R})
# oracle baseline vs joinpol at g4
orc=sum(min(m('g4',q),m('jp4',q)) for q in Q); print('oracle g4 base/jp',round(orc,3), 'delta',round(orc-tot('g4'),3))
# null oracle: g4_a vs g4_b (same config) selection bias
nul=sum(min(m('g4',q),m('g4b',q)) for q in Q); print('null oracle g4a/g4b', round(nul,3),'vs mean of two',round((tot('g4')+tot('g4b'))/2,3))
orc2=sum(min(m('b4',q),m('best4',q)) for q in Q); print('oracle batch4g/best',round(orc2,3),'delta',round(orc2-tot('b4'),3))
print('per-query jp4-g4 (s) and sds:')
for q in Q:
    d=m('jp4',q)-m('g4',q); d2=m('best4',q)-m('b4',q)
    print(q, round(d,3), 'sd g4 %.3f jp %.3f | g4b-g4a %.3f'%(sd('g4',q),sd('jp4',q),m('g4b',q)-m('g4',q)), '| best-b4 %.3f sd %.3f/%.3f'%(d2,sd('b4',q),sd('best4',q)))
# equal-policy gap change
g=lambda a,b: tot(a)-tot(b)/4
print('gap base',round(g('g4','g1'),3),'gap jp equal',round(g('jp4','jp1'),3),'gap jp4 vs g1base',round(g('jp4','g1'),3))
print('gap b4',round(g('b4','b1'),3),'gap best',round(g('best4','best1'),3))
# equal-policy oracle gap: per query choose policy for g4 & corresponding g1
og=sum(min(m('g4',q)-m('g1',q)/4, m('jp4',q)-m('jp1',q)/4) for q in Q); print('oracle gap per query (equal policy)',round(og,3),'delta',round(og-g('g4','g1'),3))
# excluding Q3 (g1 mode flip)
ex=[q for q in Q if q!=3]
print('jp4-g4 excl Q3', round(sum(m('jp4',q)-m('g4',q) for q in ex),3), 'oracle excl Q3', round(sum(min(0,m('jp4',q)-m('g4',q)) for q in ex),3))
