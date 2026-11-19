#!/usr/bin/env python3
# every scan preceded by [PARTITION][CONCAT]: CONCAT ms, scan ms, DF or not; sums per query (iteration 3 and median over hot iters 2..5)
import sys,glob,collections,statistics
sys.path.insert(0,'/opt/dlami/nvme/dfperf/runs/profile/bin'); import qload
R='/opt/dlami/nvme/dfperf/telemetry/baseline_tip'
def pipes(s,it):
    qs=list(s['queries'].items()); qid,qev=qs[it]; t0=qev['Executing']
    plans=[p for p,q in s['plans'].items() if q==qid]
    ops={i:o for i,o in s['ops'].items() if o['plan'] in plans}
    byp=collections.defaultdict(list)
    for tid,ev in s['tasks'].items():
        pid=ev[0][3]['pipeline_uuid']['target']
        if pid not in ops: continue
        d={}
        for seq,t,k,v in ev:
            if k=='Created': d['cr']=(t-t0)/1e6
            elif k=='Finalizing': d['end']=(t-t0)/1e6
        if 'end' in d: byp[pid].append(d)
    out=[dict(lab=ops[p]['label'],cr=min(d['cr'] for d in ds),end=max(d['end'] for d in ds)) for p,ds in byp.items()]
    out.sort(key=lambda p:p['cr']); return out,(qev['Finished']-t0)/1e6
tot_df=tot_nodf=0
for q in range(1,23):
    sess=sorted(glob.glob(f'{R}/q{q}/*'))[0]; s=qload.load(sess)
    nq=len(s['queries'])
    acc=collections.defaultdict(list)
    for it in range(2,nq):
        P,wall=pipes(s,it)
        for i,p in enumerate(P):
            if not p['lab'].startswith('GPU_SCAN') or i<2: continue
            c=P[i-1]; b=P[i-2]
            if not (c['lab'].startswith('CONCAT') and b['lab'].startswith('PARTITION')): continue
            # scan chain end = start of next HASH_JOIN pipeline
            hj=[x for x in P[i+1:] if x['lab'].startswith('HASH_JOIN')][:1]
            chain=(hj[0]['cr']-p['cr']) if hj else (p['end']-p['cr'])
            acc[(c['lab'],p['lab'].split(' ->')[0],'DYNAMIC_FILTER' in p['lab'])].append((c['end']-b['end'],chain))
    line=[];sd=sn=0
    for (cl,sl,df),v in acc.items():
        C=statistics.median(x[0] for x in v); S=statistics.median(x[1] for x in v); g=min(C,S)
        if df: sd+=g
        else: sn+=g
        if g>=2: line.append(f"{cl}->{sl}{'[DF]' if df else '[noDF]'} C={C:.0f} S={S:.0f} min={g:.0f}")
    tot_df+=sd; tot_nodf+=sn
    print(f"q{q:2d} DF-sites {sd:6.1f}  noDF-sites {sn:6.1f} | "+'; '.join(line))
print(f"suite: DF sites {tot_df:.0f} ms, no-DF sites {tot_nodf:.0f} ms (min(build-CONCAT window, scan-chain), medians of hot iterations)")
