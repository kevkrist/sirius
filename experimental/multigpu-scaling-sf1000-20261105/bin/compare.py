#!/usr/bin/env python3
"""compare.py run_dir... : per-query mean hot time (iters>=2) per run, plus speedup vs the first run."""
import csv,sys,collections,statistics as st,os
runs=sys.argv[1:]; data={}
for r in runs:
    d=collections.defaultdict(list)
    for x in csv.DictReader(open(os.path.join(r,'timings.csv'))):
        if x['iter'] not in ('0','1'): d[int(x['query'])].append(float(x['seconds']))
    data[os.path.basename(r.rstrip('/'))]=d
names=list(data); base=names[0]
print('q    '+''.join(f'{n:>10}' for n in names)+''.join(f'{"x"+n:>10}' for n in names[1:]))
tot=collections.Counter()
for q in range(1,23):
    m={n:st.mean(data[n][q]) if data[n].get(q) else float('nan') for n in names}
    for n in names: tot[n]+=m[n]
    print(f'{q:<5}'+''.join(f'{m[n]:10.3f}' for n in names)+''.join(f'{m[base]/m[n]:10.2f}' for n in names[1:]))
print('TOT  '+''.join(f'{tot[n]:10.2f}' for n in names)+''.join(f'{tot[base]/tot[n]:10.2f}' for n in names[1:]))
sd={n:st.pstdev([sum(v[i] for v in data[n].values() if len(v)>i) for i in range(3)]) for n in names}
print('sd(total over hot iters): '+' '.join(f'{n}={sd[n]:.3f}' for n in names))
