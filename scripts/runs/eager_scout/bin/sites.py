#!/usr/bin/env python3
# sites.py <q...> : per DF-consumer scan, what sits between previous PARTITION end and scan start (existing Quent, CPU only)
import sys,os,collections,glob,statistics
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
        d={'res':0,'mat':0,'inb':0,'tier':None}
        for seq,t,k,v in ev:
            t=(t-t0)/1e6
            if k=='Created': d['cr']=t
            elif k=='Reserving': d['res']=v.get('requested_bytes',0); d['mat']=v.get('bytes_to_materialize',0)
            elif k=='Preparing': d.setdefault('pr',t); d['inb']=v.get('input_bytes',0); d['tier']=v.get('origin_tier')
            elif k=='Computing': d.setdefault('c0',t)
            elif k=='Finalizing': d['end']=t
            elif k=='Routing': d['gpu']=v.get('preferred_device_id')
        if 'end' in d: byp[pid].append(d)
    out=[]
    for pid,ds in byp.items():
        out.append(dict(lab=ops[pid]['label'],cr=min(d['cr'] for d in ds),end=max(d['end'] for d in ds),ds=ds))
    out.sort(key=lambda p:p['cr'])
    return out,(qev['Finished']-t0)/1e6
G=1e9
for q in sys.argv[1:]:
    sess=sorted(glob.glob(f'{R}/q{q}/*'))[0]
    s=qload.load(sess)
    P,wall=pipes(s,3)
    print(f'== q{q} wall {wall:.0f} ms')
    for i,p in enumerate(P):
        if not (p['lab'].startswith('GPU_SCAN') and 'DYNAMIC_FILTER' in p['lab']): continue
        if i<2: continue
        c=P[i-1]; b=P[i-2]
        if not c['lab'].startswith('CONCAT') : 
            print(f"  scan {p['lab'][:40]} start {p['cr']:.1f}: predecessor {c['lab'][:30]} ({c['cr']:.1f}->{c['end']:.1f}) [not a CONCAT]"); continue
        prep=[d.get('c0',d['end'])-d.get('pr',d['cr']) for d in c['ds']]
        mat=sum(d['mat'] for d in c['ds']); res=max(d['res'] for d in c['ds'])
        sd=p['ds']; sdur=p['end']-p['cr']
        # downstream: next pipelines
        nxt=P[i+1:i+4]
        hj=[x for x in P[i+1:] if x['lab'].startswith('HASH_JOIN')][:1]
        pout=sum(d['inb'] for d in nxt[0]['ds']) if nxt else 0
        # max concurrent scan reservations per gpu
        per=collections.defaultdict(list)
        for d in sd: per[d.get('gpu')].append(d)
        conc=0;resc=0
        for g,ds in per.items():
            ev=[]
            for d in ds: ev.append((d.get('pr',d['cr']),1,d['res'])); ev.append((d['end'],-1,-d['res']))
            n=0;r=0
            for t,dn,dr in sorted(ev): n+=dn; r+=dr; conc=max(conc,n); resc=max(resc,r)
        print(f"  {b['lab'][:14]:14s} end {b['end']:7.1f} | {c['lab'][:11]:11s} n={len(c['ds'])} {c['cr']:7.1f}->{c['end']:7.1f} ({c['end']-c['cr']:5.1f} ms; prep med {statistics.median(prep):5.1f} max {max(prep):5.1f}; moved {mat/G:5.2f} GB; resv/task {res/G:4.1f} GB) | "
              f"{p['lab'][:12]:12s} n={len(sd):2d} {p['cr']:7.1f}->{p['end']:7.1f} ({sdur:5.1f} ms; in {sum(d['inb'] for d in sd)/G:6.1f} GB {sd[0]['tier']}; resv/task {max(d['res'] for d in sd)/G:4.1f} GB; max conc/GPU {conc}, {resc/G:4.1f} GB) | probe-side out {pout/G:5.2f} GB | {hj[0]['lab'][:13] if hj else '-'} starts {hj[0]['cr'] if hj else 0:7.1f}")
