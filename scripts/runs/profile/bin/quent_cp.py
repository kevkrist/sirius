#!/usr/bin/env python3
# Backward critical-chain walk over Quent tasks: from query Finished, repeatedly take the task that ended last before t, jump to its Created.
import sys,json,glob,collections,os,statistics as st
sys.path.insert(0,os.path.dirname(__file__)); import qload
def walk(sess,verbose=False):
    s=qload.load(sess); res=[]
    for qi,(qid,qev) in enumerate(s['queries'].items()):
        if qi==0 or 'Finished' not in qev: continue
        t0=qev['Executing']; tend=qev['Finished']
        plans={p for p,q in s['plans'].items() if q==qid}
        tasks=[]
        for tid,ev in s['tasks'].items():
            pid=ev[0][3]['pipeline_uuid']['target']
            if s['ops'][pid]['plan'] not in plans: continue
            d={k:t for seq,t,k,v in ev if k!='Computing'}; c=[t for seq,t,k,v in ev if k=='Computing']
            if 'Finalizing' in d and c: tasks.append((d['Finalizing'],d['Created'],d.get('Preparing'),c[0],s['ops'][pid]['label']))
        tasks.sort()
        t=tend; chain=[]; gap=0
        while True:
            c=[x for x in tasks if x[0]<=t+50_000]
            if not c: gap+=t-t0; break
            e,cr,pr,c0,lab=c[-1]
            if cr>=t: # avoid loops
                tasks.remove(c[-1]); continue
            if t-e>0: gap+=t-e
            chain.append(dict(pipe=lab,created=(cr-t0)/1e6,end=(e-t0)/1e6,queue=(pr-cr)/1e6,prep=(c0-pr)/1e6,comp=(e-c0)/1e6,gap_after=max(0,(t-e)/1e6)))
            t=cr
        res.append(dict(iter=qi,wall=(tend-t0)/1e6,gap=gap/1e6,chain=chain[::-1]))
    return res
if __name__=='__main__':
    out={}
    for lab in sys.argv[2:]:
        for qn in range(1,23):
            for sess in glob.glob(f'/opt/dlami/nvme/dfperf/telemetry/{lab}/q{qn}/*/'):
                out.setdefault(str(qn),[]).extend(walk(sess))
    json.dump(out,open(sys.argv[1],'w'),indent=1)
