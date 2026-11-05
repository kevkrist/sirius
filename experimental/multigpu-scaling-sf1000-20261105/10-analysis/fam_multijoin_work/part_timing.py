#!/usr/bin/env python3
"""part_timing.py <quent> <qnum> <scan_pid> <part_pid> <concat_pid> [iters]: for the post-scan exchange, per hot iter:
#tasks, scan last finalize, partition created p10/p50/p90, started (preparing) p10/p50/p90, partition comp thread-ms,
mean per-task partition comp ms, concat tasks, concat GPU-prep thread-ms, concat span (ms from query start)."""
import csv,sys,collections,statistics as st
d,qn,sp,pp,cp=sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4]),int(sys.argv[5])
iters=[int(x) for x in (sys.argv[6] if len(sys.argv)>6 else '2,3,4').split(',')]
Q={r[0]:int(r[2]) for r in csv.reader(open(d+'/queries.csv')) if r[0].startswith('tpch')}
T=collections.defaultdict(list)
for r in csv.DictReader(open(d+'/tasks.csv')):
    if r['query'] not in [f'tpch_q{qn}_iter{i}' for i in iters]: continue
    pid=int(r['pipeline_type'].split()[-1])
    if pid not in (sp,pp,cp): continue
    try: T[(r['query'],pid)].append({k:int(r[k]) for k in ('created','preparing','first_compute','finalize')}|{'origin':r['origin'],'bytes':int(r['in_bytes'] or 0)})
    except: pass
def pct(v,p): v=sorted(v); return v[min(len(v)-1,int(p*len(v)))]
for i in iters:
    q=f'tpch_q{qn}_iter{i}'; s0=Q[q]; ms=lambda x:(x-s0)/1e6
    S,P,C=T[(q,sp)],T[(q,pp)],T[(q,cp)]
    if not S or not P: print(q,'missing',len(S),len(P)); continue
    sl=max(ms(t['finalize']) for t in S); sf=min(ms(t['preparing']) for t in S)
    cr=[ms(t['created']) for t in P]; stt=[ms(t['preparing']) for t in P]; comp=[(t['finalize']-t['first_compute'])/1e6 for t in P]
    pend=max(ms(t['finalize']) for t in P)
    cg=sum((t['first_compute']-t['preparing'])/1e6 for t in C if t['origin']=='GPU')
    cspan=(min(ms(t['preparing']) for t in C), max(ms(t['finalize']) for t in C)) if C else (0,0)
    before=sum(1 for x in stt if x<sl)
    print(f"{q}: scan n={len(S)} {sf:.0f}-{sl:.0f} | part n={len(P)} created p10/50/90={pct(cr,.1):.0f}/{pct(cr,.5):.0f}/{pct(cr,.9):.0f} started p10/50/90={pct(stt,.1):.0f}/{pct(stt,.5):.0f}/{pct(stt,.9):.0f} started<scan_end={before} last_end={pend:.0f} comp={sum(comp):.0f}tms ({st.mean(comp):.2f}/task) | concat n={len(C)} GPUprep={cg:.0f}tms span {cspan[0]:.0f}-{cspan[1]:.0f} in={sum(t['bytes'] for t in C)/1e9:.1f}GB")
