#!/usr/bin/env python3
"""quent_order.py <telemetry label dir> [q ...] : ordering check from Quent task timelines, hot windows (queries 2..N of each session).
For every HASH_JOIN pipeline with a build chain ... PARTITION(p) -> CONCAT(c) -> join: 
  part_end  = last end of PARTITION(p) tasks (tip publication happens inside the last task),
  conc_end  = last end of CONCAT(c) tasks (prototype local publication happens inside the last CONCAT sink),
  consumers = scan pipelines with DYNAMIC_FILTER feeding the join's probe side (transitively, excluding the build subtree).
Reports per join: min over windows of (first consumer task Preparing - part_end) and (- conc_end); count of windows with a consumer task
that started before conc_end / part_end. Also site timings (medians): part first c0, part_end, conc first preparing, conc_end, first consumer."""
import sys,glob,re,collections,os,statistics as st
sys.path.insert(0,'/opt/dlami/nvme/dfperf/runs/profile/bin'); import qload
lab=sys.argv[1]; qsel=sys.argv[2:]
for qd in sorted(glob.glob(lab+'/q*'),key=lambda x:int(os.path.basename(x)[1:])):
    qn=os.path.basename(qd)[1:]
    if qsel and qn not in qsel: continue
    R=collections.defaultdict(list)
    for sess in sorted(glob.glob(qd+'/*/')):
        s=qload.load(sess); qs=list(s['queries'].items())
        for qi,(qid,qev) in enumerate(qs):
            if qi<2 or 'Finished' not in qev or 'Executing' not in qev: continue
            t0=qev['Executing']
            plans=[p for p,q in s['plans'].items() if q==qid]
            ops={i:o for i,o in s['ops'].items() if o['plan'] in plans}
            inc=collections.defaultdict(list)
            for p in plans:
                for a,b in s['edges'][p]:
                    oa,la=s['ports'][a]; ob,lb=s['ports'][b]; inc[ob].append((oa,lb))
            T=collections.defaultdict(list)
            for tid,ev in s['tasks'].items():
                pid=ev[0][3]['pipeline_uuid']['target']
                if pid not in ops: continue
                d={}
                for seq,t,k,v in ev:
                    if k=='Created': d['created']=t
                    elif k=='Preparing': d['preparing']=t
                    elif k=='Computing': d.setdefault('c0',t)
                    elif k=='Finalizing': d['end']=t
                if 'end' in d and 'preparing' in d: T[pid].append(d)
            def anc(pid,probe_only,seen):
                res=[]
                for src,lb in inc.get(pid,[]):
                    if probe_only and lb=='build_receiver': continue
                    if src in seen: continue
                    seen.add(src); res.append(src); res+=anc(src,False,seen)
                return res
            for jp,o in ops.items():
                m=re.match(r'HASH_JOIN\((\d+)\)',o['label'])
                if not m: continue
                b=[src for src,lb in inc[jp] if lb=='build_receiver']
                if not b: continue
                conc=b[0] if ops[b[0]]['label'].startswith('CONCAT') else None
                part=None; cur=b[0]
                for _ in range(3):
                    if ops[cur]['label'].startswith('PARTITION'): part=cur; break
                    nx=[src for src,lb in inc.get(cur,[])]
                    if len(nx)!=1: break
                    cur=nx[0]
                if part is None or not T[part]: continue
                bsub=set(anc(b[0],False,set()))|{b[0]}
                cons=[x for x in anc(jp,True,set()) if x not in bsub and 'DYNAMIC_FILTER' in ops[x]['label'] and 'GPU_SCAN' in ops[x]['label']]
                pe=max(d['end'] for d in T[part]); pc0=min(d.get('c0',d['preparing']) for d in T[part])
                ce=max(d['end'] for d in T[conc]) if conc and T[conc] else None
                cp=min(d['preparing'] for d in T[conc]) if conc and T[conc] else None
                cst=[d['preparing'] for x in cons for d in T[x]]
                ccr=[d['created'] for x in cons for d in T[x] if 'created' in d]
                f=lambda x: (x-t0)/1e6 if x is not None else None
                R[int(m.group(1))].append(dict(part_c0=f(pc0),part_end=f(pe),conc_prep=f(cp),conc_end=f(ce),first_cons=f(min(cst)) if cst else None,
                    first_cons_created=f(min(ccr)) if ccr else None,
                    n_before_conc=sum(1 for x in cst if ce is not None and x<ce),n_before_part=sum(1 for x in cst if x<pe),ncons=len(cons),
                    labels=','.join(ops[x]['label'].split(' ')[0] for x in cons), wall=(qev['Finished']-t0)/1e6))
    for J,rs in sorted(R.items()):
        md=lambda k: (lambda v: '%.1f'%st.median(v) if v else '-')([r[k] for r in rs if r[k] is not None])
        g1=[r['first_cons']-r['part_end'] for r in rs if r['first_cons'] is not None]
        g2=[r['first_cons']-r['conc_end'] for r in rs if r['first_cons'] is not None and r['conc_end'] is not None]
        print('q%s j%-3d win=%d wall=%s | PART c0 %s end %s | CONCAT prep %s end %s | first consumer %s (created %s) [%s] | min gap after PART end %s, after CONCAT end %s | windows with consumer task before PART end %d, before CONCAT end %d'%(
            qn,J,len(rs),md('wall'),md('part_c0'),md('part_end'),md('conc_prep'),md('conc_end'),md('first_cons'),md('first_cons_created'),rs[0]['labels'],
            '%.1f'%min(g1) if g1 else '-','%.1f'%min(g2) if g2 else '-',sum(1 for r in rs if r['n_before_part']),sum(1 for r in rs if r['n_before_conc'])))
