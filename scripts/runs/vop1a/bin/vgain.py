#!/usr/bin/env python3
"""gain.py <label> [q ...] : per DF site, tip (A1+A2) vs proto (B1+B2), medians over hot iterations (ms).
F  = finish taken off the Publishing Task = pubtask(tip) - pubtask(proto)
C  = build CONCAT window at tip (the slack);  F' = continuation run time;  S = site + CONCAT (PARTITION start -> CONCAT end)
realized = S(tip) - S(proto);  ideal = min(F, C) (no contention);  lost = ideal - realized
"""
import sys,statistics as st

import importlib.util
spec=importlib.util.spec_from_file_location('sitepy','/opt/dlami/nvme/dfperf/runs/vop1a/bin/vsite.py'); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
label=sys.argv[1]; qs=[int(x) for x in sys.argv[2:]] or list(range(1,23))
med=lambda x: st.median(x) if x else float('nan')
def merged(arms,q):
    import collections
    rows=collections.defaultdict(lambda: collections.defaultdict(list))
    for a in arms:
        rr,_=m.analyze(label,a,q)
        for J,r in rr.items():
            for k,v in r.items():
                if isinstance(v,list): rows[J][k]+=v
                else: rows[J][k]=v
    return rows
print('| q | join | site | kinds | bloom MiB | F | C (slack) | F\' | F\'/F | CONCAT work\' - C | S tip | S proto | realized | ideal min(F,C) | realized/ideal | min gap scan-visible ms | early scans |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
tot=dict(real=0,ideal=0,F=0)
for q in qs:
    if str(q) not in m.summ or not any(j['started'] for j in m.summ[str(q)]['joins']): continue
    import os; PA=os.environ.get('ARM_A','A'); A=merged([PA+'1',PA+'2'],q); B=merged(['B1','B2'],q)
    for J in A:
        a=A[J]; b=B.get(J)
        if not b or not b['pubtask']: continue
        F=med(a['pubtask'])-med(b['pubtask']); C=med(a['concat']) if a['concat'] else 0.0
        Sa=med(a['site_to_concat_end']) if a['site_to_concat_end'] else med(a['site']); Sb=med(b['site_to_concat_end']) if b['site_to_concat_end'] else med(b['site'])
        Fp=med(b['cont']) if b['cont'] else float('nan')
        # CONCAT's own work at proto: CONCAT end is max(work end, continuation end); if the continuation ended first, the window is the work
        cw=(med(b['concat'])-C) if b['concat'] else float('nan')
        real=Sa-Sb; ideal=min(F,C)
        tot['real']+=real; tot['ideal']+=ideal; tot['F']+=F
        gap=min(b['gapC']) if b['gapC'] else float('nan')
        print(f"| {q} | {J} | {a['site_label']} | {a['kinds']} | {a['bloom_mb']:.0f} | {F:.1f} | {C:.1f} | {Fp:.1f} | {Fp/F if F>0.05 else float('nan'):.2f} | {cw:+.1f} | {Sa:.1f} | {Sb:.1f} | {real:+.1f} | {ideal:.1f} | {real/ideal if ideal>0.5 else float('nan'):.2f} | {gap:.2f} | {sum(b['early'])} of {len(b['early'])} |")
print(f"| sum | | | | | {tot['F']:.0f} | | | | | | | {tot['real']:+.0f} | {tot['ideal']:.0f} | {tot['real']/tot['ideal'] if tot['ideal'] else 0:.2f} | | |")
