#!/usr/bin/env python3
"""nsys_map_time.py <nsys_run_dir>: map each rep.N.sqlite to the Quent query label(s) it covers by absolute time.
Needs <dir>/quent (bin/quent_flatten.py output). Writes <dir>/nsys/map.csv: query,iter,report,sqlite,overlap_ms,query_ms_in_nsys,range_start_ns,range_end_ns (report-relative sirius::query window; filter all events to it)"""
import sqlite3,csv,sys,os,glob
d=sys.argv[1]
Q=[(r['query'],int(r['init']),int(r['finished'])) for r in csv.DictReader(open(f"{d}/quent/queries.csv")) if r['finished']]
rows=[]
for f in sorted(glob.glob(f"{d}/nsys/rep.*.sqlite"),key=lambda f:int(f.split('rep.')[1].split('.')[0])):
    c=sqlite3.connect(f); S=dict(c.execute("select id,value from StringIds"))
    t0=c.execute("select utcEpochNs from TARGET_INFO_SESSION_START_TIME").fetchone()[0]
    qid=[k for k,v in S.items() if v=='sirius::query']
    rngs=[(s+t0,e+t0,s,e) for s,e in c.execute(f"select start,end from NVTX_EVENTS where (text='sirius::query' or textId in ({','.join(map(str,qid)) or -1})) and end is not null")]
    for s,e,rs,re_ in rngs:
        best=max(Q,key=lambda q:min(e,q[2])-max(s,q[1]))
        ov=(min(e,best[2])-max(s,best[1]))/1e6
        lab=best[0]; qn=int(lab.split('_q')[1].split('_')[0]); it=int(lab.split('iter')[1])
        rows.append((qn,it,f.replace('.sqlite','.nsys-rep'),f,round(ov,1),round((e-s)/1e6,1),rs,re_))
        print(os.path.basename(f),lab,f"overlap={ov:.0f}ms nsys_query_ms={(e-s)/1e6:.0f}")
with open(f"{d}/nsys/map.csv","w") as fh:
    fh.write("query,iter,report,sqlite,overlap_ms,query_ms_in_nsys,range_start_ns,range_end_ns\n")
    for r in sorted(rows): fh.write(",".join(map(str,r))+"\n")
