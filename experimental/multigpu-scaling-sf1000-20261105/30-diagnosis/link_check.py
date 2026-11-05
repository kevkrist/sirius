#!/usr/bin/env python3
"""Per-query: family per-phase link price vs X3 per-query hard floor vs gpupin measured delta (g4 - gpupin)."""
import csv
def hot(run):
    d={}
    for r in csv.DictReader(open(f"/home/ubuntu/sirius-scaling-20261105/{run}/timings.csv")):
        if r['query'].isdigit() and int(r['iter'])>=2: d.setdefault(int(r['query']),[]).append(float(r['seconds']))
    return {k:sum(v)/len(v) for k,v in d.items()}
g1=hot('00-baseline/g1_a'); g4=hot('00-baseline/g4_a'); gp=hot('20-experiments/x_g4_gpupin')
fam={1:61,2:148.5,3:178,4:81,5:206,6:122,7:225,8:319,9:340,10:253,11:95.3,12:262,13:340,14:228,15:169,16:76.2,17:334,18:121,19:384,20:294,21:385,22:60.7}
hw={1:0,2:.15,3:0,4:.05,5:.20,6:.12,7:.20,8:.33,9:.19,10:.15,11:.10,12:.23,13:.07,14:.18,15:.16,16:.04,17:.36,18:0,19:.39,20:.32,21:.21,22:.03}
conf={1,3,18,10,16,21,22}  # gpupin downgrade errors (1,3,18) or memory-pressure regressions (10,16,21,22)
S=dict(fam=0,hw=0,gp=0,min=0)
print("Q   gap   fam_link  hw_floor  gpupin_d  min(fam,gpupin)")
for q in range(1,23):
    gap=(g4[q]-g1[q]/4)*1000; d=(g4[q]-gp[q])*1000
    m = fam[q] if q in conf else min(fam[q], max(d,0))
    S['fam']+=fam[q]; S['hw']+=hw[q]*1000; S['min']+=m
    if q not in conf: S['gp']+=d
    print(f"Q{q:<3}{gap:6.0f} {fam[q]:8.0f} {hw[q]*1000:8.0f} {d:9.0f}{' (confounded)' if q in conf else ''} {m:8.0f}")
print({k:round(v) for k,v in S.items()})
