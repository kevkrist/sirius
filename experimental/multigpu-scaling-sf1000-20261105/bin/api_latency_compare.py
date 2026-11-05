#!/usr/bin/env python3
"""api_latency_compare.py <mapA.csv> <mapB.csv> [query...]: per CUDA runtime API (within each query's iter2 window), total thread-ms, call count,
mean/p90 us for run A vs B. Summed over the listed queries (default all 22)."""
import sqlite3,csv,sys,collections
def load(mp,qs):
    agg=collections.defaultdict(list)
    for r in csv.DictReader(open(mp)):
        if r['iter']!='2' or (qs and int(r['query']) not in qs): continue
        c=sqlite3.connect(r['sqlite']); S=dict(c.execute("select id,value from StringIds"))
        for n,d in c.execute("select nameId,end-start from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?",(int(r['range_start_ns']),int(r['range_end_ns']))):
            agg[S[n].split('_v')[0]].append(d)
    return agg
qs=[int(x) for x in sys.argv[3:]]
A=load(sys.argv[1],qs); B=load(sys.argv[2],qs)
print(f"{'api':28}{'nA':>9}{'nB':>9}{'msA':>9}{'msB':>9}{'meanA':>8}{'meanB':>8}{'p90A':>8}{'p90B':>8}  (us)")
for k in sorted(set(A)|set(B),key=lambda k:-(sum(B.get(k,[]))))[:18]:
    a=sorted(A.get(k,[0])); b=sorted(B.get(k,[0]))
    print(f"{k[:28]:28}{len(a):9}{len(b):9}{sum(a)/1e6:9.0f}{sum(b)/1e6:9.0f}{sum(a)/len(a)/1e3:8.1f}{sum(b)/len(b)/1e3:8.1f}{a[int(len(a)*.9)]/1e3:8.1f}{b[int(len(b)*.9)]/1e3:8.1f}")
