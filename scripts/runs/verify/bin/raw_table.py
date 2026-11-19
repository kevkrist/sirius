#!/usr/bin/env python3
# Recompute timing from raw runtimes.csv. Labels given as name=dir. Prints per-query hot iterations per label.
import glob, csv, sys, statistics as st, re, collections
labels=dict(a.split('=') for a in sys.argv[1:])
D=collections.defaultdict(dict)
for lab,d in labels.items():
    for f in sorted(glob.glob(d+'/tpch_*_q*/csv/runtimes.csv')):
        q=int(re.search(r'_q(\d+)/csv',f).group(1))
        hot=[float(r['runtime_s'])*1e3 for r in csv.DictReader(open(f)) if r['query']!='TOTAL' and int(r['iteration'])>=1]
        D[q].setdefault(lab,[]).extend(hot)   # reruns of the same label/q are pooled (flagged by count)
import json
json.dump({q:D[q] for q in D},open('/opt/dlami/nvme/dfperf/runs/verify/raw_hot.json','w'))
labs=list(labels)
print('q | '+' | '.join(f'{l} hot ms' for l in labs))
for q in sorted(D):
    print(q,'|',' | '.join(' '.join('%.0f'%x for x in D[q].get(l,[])) for l in labs))
