#!/usr/bin/env python3
# raw.py <tag> [q...] : raw per-process iteration times (ms), in run order
import glob,csv,sys,os
tag=sys.argv[1]; qs=sys.argv[2:]
rows=[]
for d in glob.glob('/opt/dlami/nvme/dfperf/runs/vsgb/%s_*_[0-9]'%tag):
    for f in glob.glob(d+'/tpch_*/csv/runtimes.csv'):
        r=[(x['query'],float(x['runtime_s'])*1e3) for x in csv.DictReader(open(f)) if x['query']!='TOTAL']
        if r and (not qs or r[0][0].lstrip('q') in qs): rows.append((r[0][0],os.path.getmtime(f),os.path.basename(d),' '.join('%.0f'%x[1] for x in r)))
for q,t,d,s in sorted(rows,key=lambda x:(int(x[0].lstrip('q')),x[1])): print(q,d,s)
