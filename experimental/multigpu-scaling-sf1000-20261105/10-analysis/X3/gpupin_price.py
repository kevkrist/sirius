#!/usr/bin/env python3
"""gpupin_price.py: g4 host-pin vs g4 gpu-pin (x_g4_gpupin) on queries whose gpupin run is clean (excludes Q1,Q3,Q18:
thousands of downgrade 'stream belongs to CUDA device' errors / column-count-mismatch warnings in that run)."""
import csv, collections
def tim(p):
    t = collections.defaultdict(list)
    for r in csv.DictReader(open(p)):
        if r['query'].isdigit() and int(r['iter']) >= 2: t[int(r['query'])].append(float(r['seconds']))
    return {q: sum(v) / len(v) for q, v in t.items()}
B = '/home/ubuntu/sirius-scaling-20261105/'
g1 = tim(B + '00-baseline/g1_a/timings.csv'); g4 = tim(B + '00-baseline/g4_a/timings.csv'); gp = tim(B + '20-experiments/x_g4_gpupin/timings.csv')
fl = {int(r['query']): r for r in csv.DictReader(open('floors.csv'))}
cs = {}
for l in open('g4_interior_scan2.txt'):
    p = l.split()
    if p and p[0].isdigit(): cs[int(p[0])] = float(p[5]) * 20.04 / 21.777 / 1000
clean = [q for q in range(1, 23) if q not in (1, 3, 18)]
S = lambda d: sum(d[q] for q in clean)
print(f"clean n={len(clean)} g1={S(g1):.2f} g4={S(g4):.2f} gpupin={S(gp):.2f} ideal={S(g1)/4:.2f} saved={S(g4)-S(gp):.2f} "
      f"scan-copy(g4,unprof-eq)={sum(cs[q] for q in clean):.2f} hw-floor-share={sum(float(fl[q]['hw_share']) for q in clean):.2f}")
print(f"gap clean {S(g4)-S(g1)/4:.2f} -> with gpupin {S(gp)-S(g1)/4:.2f}")
for q in clean:
    print(q, f"g4 {g4[q]:.3f} gpupin {gp[q]:.3f} saved {g4[q]-gp[q]:+.3f} scancopy {cs[q]:.3f} ratio {(g4[q]-gp[q])/cs[q]:.2f} ideal {g1[q]/4:.3f}")
