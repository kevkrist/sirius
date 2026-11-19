#!/usr/bin/env python3
# quent_df.py <telemetry_label_dir> <out.json>  : per DF-building join timelines from Quent (hot iterations = queries 2..N of a session)
import sys,json,glob,re,statistics as st,collections,os
sys.path.insert(0,os.path.dirname(__file__)); import qload
R='/opt/dlami/nvme/dfperf'
summ=json.load(open(R+'/runs/baseline/logpass/df_summary.json'))
med=lambda x: st.median(x) if x else None
def union(iv):
    iv=sorted(iv); tot=0; ce=None; cs=None
    for a,b in iv:
        if ce is None or a>ce:
            if ce is not None: tot+=ce-cs
            cs,ce=a,b
        else: ce=max(ce,b)
    if ce is not None: tot+=ce-cs
    return tot
def analyze(qn,sess):
    s=qload.load(sess); out=[]
    qs=list(s['queries'].items())
    joins=[j for j in summ[str(qn)]['joins'] if j['started']]
    for qi,(qid,qev) in enumerate(qs):
        if qi==0 or 'Finished' not in qev: continue
        t0=qev['Executing']; wall=(qev['Finished']-t0)/1e6
        plans=[p for p,q in s['plans'].items() if q==qid]
        ops={i:o for i,o in s['ops'].items() if o['plan'] in plans}
        inc=collections.defaultdict(list)
        for p in plans:
            for a,b in s['edges'][p]:
                oa,la=s['ports'][a]; ob,lb=s['ports'][b]; inc[ob].append((oa,lb))
        # tasks per pipeline
        T=collections.defaultdict(list); downg=collections.Counter()
        for tid,ev in s['tasks'].items():
            pid=ev[0][3]['pipeline_uuid']['target']
            if pid not in ops: continue
            d=dict(ops=[])
            for seq,t,k,v in ev:
                if k=='Created': d['created']=t
                elif k=='Reserving': d.setdefault('reserving',t); d['req']=v.get('requested_bytes'); d['nres']=d.get('nres',0)+1
                elif k=='Preparing': d['preparing']=t; d['in_bytes']=v.get('input_bytes'); d['tier']=v.get('origin_tier')
                elif k=='Computing':
                    d.setdefault('c0',t); d['ops'].append((v.get('current_operator_id'),t)); d['dev']=v.get('reservation',{}).get('target')
                elif k=='Finalizing': d['end']=t; d['ok']=v.get('success')
                elif k=='Routing': d['gpu']=v.get('preferred_device_id')
                elif k not in('Queued',): downg[k]+=1; d['other']=d.get('other',[])+[k]
            if 'c0' in d and 'end' in d: T[pid].append(d)
        def find(pat):
            return [i for i,o in ops.items() if re.search(pat,o['label'])]
        def anc_scans(pid,probe_only,seen=None):
            seen=seen if seen is not None else set(); res=[]
            for src,lb in inc.get(pid,[]):
                if probe_only and lb=='build_receiver': continue
                if src in seen: continue
                seen.add(src)
                if 'GPU_SCAN' in ops[src]['label']: res.append(src)
                res+=anc_scans(src,probe_only,seen)
            return res
        rec=dict(q=qn,iter=qi,wall_ms=wall,joins=[],other_task_states=dict(downg))
        allbuild=[]
        for j in joins:
            J=j['join']
            jp=find(r'HASH_JOIN\(%d\)'%J) or find(r'\(%d\)'%J)
            if not jp: rec['joins'].append(dict(join=J,err='no pipeline')); continue
            jp=jp[0]
            b=[src for src,lb in inc[jp] if lb=='build_receiver']
            # walk build chain down to the PARTITION / BUILD_FILTER
            chain=[]; cur=b[0] if b else None
            while cur is not None and len(chain)<6:
                chain.append(cur)
                if 'BUILD_FILTER' in ops[cur]['label'] or 'GPU_SCAN' in ops[cur]['label']: break
                nx=[src for src,lb in inc.get(cur,[])]; cur=nx[0] if len(nx)==1 else None
            bp=[c for c in chain if re.match(r'PARTITION',ops[c]['label'])]
            bf=[c for c in chain if 'BUILD_FILTER' in ops[c]['label']]
            site=(bf or bp or [None])[0]; jr_part=bp[0] if (bf and bp) else None
            jr=dict(join=J,layout=j['layout'],broadcast=j.get('broadcast'),chain=[ops[c]['label'] for c in chain],site=ops[site]['label'] if site else None,
                    bloom_bytes=sum(int(k['size']) for k in j['keys'] if k['kind']=='bloom'),kinds='+'.join(k['kind'] for k in j['keys']),log_batches=j['started'][0]['batches'])
            if site is None: rec['joins'].append(jr); continue
            ts=sorted(T[site],key=lambda d:d['end'])
            if not ts: jr['err']='no tasks'; rec['joins'].append(jr); continue
            comp=[(d['end']-d['c0'])/1e6 for d in ts]
            last=ts[-1]; tpub=last['end']
            jr.update(n_tasks=len(ts),comp_med=med(comp),comp_last=comp[-1],comp_max=max(comp),comp_sum=sum(comp),
                      last_is_max=(comp[-1]==max(comp)),
                      resv_wait_max=max((d['preparing']-d['reserving'])/1e6 for d in ts),
                      resv_wait_sum=sum((d['preparing']-d['reserving'])/1e6 for d in ts),
                      prep_max=max((d['c0']-d['preparing'])/1e6 for d in ts),
                      queue_max=max((d['reserving']-d['created'])/1e6 for d in ts),
                      nres_max=max(d.get('nres',1) for d in ts), tiers=sorted(set(d.get('tier') for d in ts)),
                      gpus=sorted(set(d.get('gpu') for d in ts)), other=[d.get('other') for d in ts if d.get('other')],
                      first_start=(ts[0]['c0']-t0)/1e6 if True else None, first_c0=(min(d['c0'] for d in ts)-t0)/1e6,
                      tpub=(tpub-t0)/1e6, build_union=union([(d['c0'],d['end']) for d in ts])/1e6,
                      in_bytes=sum(d.get('in_bytes') or 0 for d in ts))
            allbuild+=[(d['c0'],d['end']) for d in ts]
            # consumers
            prim=set(anc_scans(jp,True)); alls=anc_scans(jp,False)
            # exclude the build subtree of this join
            bsub=set(anc_scans(b[0],False))|{b[0]} if b else set()
            cons=[]
            for sc in alls:
                if sc in bsub: continue
                lab=ops[sc]['label']
                if 'DYNAMIC_FILTER' not in lab: continue
                st_=sorted(T[sc],key=lambda d:d['preparing'])
                for d in st_: d['c0']=d['preparing']
                if not st_: continue
                before=[d for d in st_ if d['c0']<tpub]
                ob=[(bb['Constructed'],bb.get('bytes',0)) for bb in s['batches'].values() if bb.get('prod')==sc and 'Constructed' in bb]
                cons.append(dict(scan=lab,primary=sc in prim,n_tasks=len(st_),first_c0=(st_[0]['c0']-t0)/1e6,last_end=(max(d['end'] for d in st_)-t0)/1e6,
                    tasks_started_before_pub=len(before),tasks_ended_before_pub=len([d for d in st_ if d['end']<=tpub]),
                    in_bytes=sum(d.get('in_bytes') or 0 for d in st_),in_bytes_started_before=sum(d.get('in_bytes') or 0 for d in before),
                    comp_sum=sum((d['end']-d['c0'])/1e6 for d in st_),
                    comp_before=sum((min(d['end'],tpub)-d['c0'])/1e6 for d in before),
                    out_batches=len(ob),out_bytes=sum(x[1] for x in ob),out_bytes_before=sum(x[1] for x in ob if x[0]<tpub),out_batches_before=len([x for x in ob if x[0]<tpub]),
                    gap_pub_to_first=(st_[0]['c0']-tpub)/1e6,gap_pub_to_created=(min(d['created'] for d in st_)-tpub)/1e6))
            jr['consumers']=cons
            rec['joins'].append(jr)
        rec['build_union_all']=union(allbuild)/1e6
        out.append(rec)
    return out
if __name__=='__main__':
    lab,outp=sys.argv[1],sys.argv[2]; res=[]
    for qn in range(1,23):
        if not summ[str(qn)]['joins'] or not any(j['started'] for j in summ[str(qn)]['joins']): continue
        for sess in sorted(glob.glob(f'{lab}/q{qn}/*/')):
            res+=analyze(qn,sess)
    json.dump(res,open(outp,'w'),indent=1); print(len(res),'records')
