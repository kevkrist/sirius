#!/usr/bin/env python3
# For a DF build site: is anything else running while its Publishing Task runs? (adversarial check of "serial chain, 1:1")
import sys,glob,collections
sys.path.insert(0,'/opt/dlami/nvme/dfperf/runs/profile/bin')
import qload
def union(iv):
    iv=sorted(iv); tot=0; ce=None
    for a,b in iv:
        if ce is None or a>ce: tot+=b-a; ce=b
        elif b>ce: tot+=b-ce; ce=b
    return tot
for spec in sys.argv[1:]:
    lab,q,site=spec.split(':')
    sess=glob.glob(f'/opt/dlami/nvme/dfperf/telemetry/{lab}/q{q}/*/')[0]
    s=qload.load(sess); ops=s['ops']
    qids=list(s['queries'])
    for qi in qids[1:]:
        qev=s['queries'][qi]; t0=qev['Executing']; t1=qev['Finished']
        plans=[p for p,qq in s['plans'].items() if qq==qi]
        T=collections.defaultdict(list)
        for tid,ev in s['tasks'].items():
            pid=ev[0][3]['pipeline_uuid']['target']
            if pid not in ops or ops[pid]['plan'] not in plans: continue
            d={}
            for seq,t,k,v in ev:
                if k=='Created': d['created']=t
                elif k=='Preparing': d['prep']=t
                elif k=='Computing': d.setdefault('c0',t)
                elif k=='Finalizing': d['end']=t
            if 'end' in d and 'prep' in d: T[pid].append(d)
        sp=[p for p in T if ops[p]['label']==site]
        if not sp: print(spec,'site not found',sorted(set(ops[p]['label'] for p in T))[:6]); break
        sp=sp[0]; pub=max(T[sp],key=lambda d:d['end']); a,b=pub['c0'],pub['end']
        others=[]; labs=collections.Counter()
        for p in T:
            if p==sp: continue
            for d in T[p]:
                x,y=max(d['prep'],a),min(d['end'],b)
                if y>x: others.append((x,y)); labs[ops[p]['label'][:40]]+=(y-x)/1e6
        print(f"{lab} q{q} {site}: wall {(t1-t0)/1e6:.0f} ms, publishing task {(b-a)/1e6:.1f} ms @{(a-t0)/1e6:.0f}; other-pipeline tasks active for {union(others)/1e6:.1f} ms of it; {dict((k,round(v,1)) for k,v in labs.most_common(3))}")
