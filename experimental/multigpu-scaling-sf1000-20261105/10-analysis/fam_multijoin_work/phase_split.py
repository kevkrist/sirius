#!/usr/bin/env python3
"""phase_split.py <crit prefix e.g. crit_q>: split critpath excess into pre-scan / lineitem scan / post-scan phases."""
import re,collections,sys
main={5:24,7:18,8:33,9:24}
pref=sys.argv[1]
for q in [5,7,8,9]:
    C=collections.Counter()
    for l in open(f'{pref}{q}.txt'):
        m=re.match(r'(P\d+|gap)\s+(.*?)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)$',l.rstrip())
        if not m or m.group(1)=='gap': continue
        p,lab,a,b,a4,ex=m.groups(); pid=int(p[1:])
        ph='pre' if pid<main[q] else ('scan' if pid==main[q] else 'post')
        cat='SCAN' if lab.startswith('GPU_SCAN') else lab.split('(')[0]
        C[(ph,cat)]+=float(ex); C[(ph,'A')]+=float(a); C[(ph,'B')]+=float(b)
    for ph in ['pre','scan','post']:
        print(f" Q{q} {ph:5} g1={C[(ph,'A')]:6.0f} g4={C[(ph,'B')]:6.0f} excess={C[(ph,'B')]-C[(ph,'A')]/4:5.0f} | "+' '.join(f"{k[1]}={v:.0f}" for k,v in C.items() if k[0]==ph and k[1] not in 'AB' and abs(v)>=2))
