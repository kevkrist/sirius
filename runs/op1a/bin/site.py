#!/usr/bin/env python3
"""site.py <label> <arm[,arm]> [q ...] : per DF join site, from Quent (+ [df_async] log lines for proto arms).
Hot iterations = queries 2..N of each session (iteration 0 warm, 1 first hot dropped).
Columns (medians over hot iterations, ms):
  pubtask  = duration (Computing->end) of the last-ending build-site task (the Publishing Task)
  site     = build-site pipeline: first task created -> last task end
  concat   = build CONCAT pipeline: first task created -> last task end (proto: includes the await)
  cont     = continuation run time ([df_async] run_ms); await = max waited_ms of the join point
  gapC/gapP= first consumer scan Created / Preparing minus publication (min over iterations and scans);
             publication = visible_epoch_ns (proto, logged) or end of the Publishing Task (tip, proxy)
  early    = consumer scan tasks created before publication (sum over iterations)
"""
import sys,os,re,glob,json,collections,statistics as st
sys.path.insert(0,'/opt/dlami/nvme/dfperf/runs/profile/bin'); import qload
R='/opt/dlami/nvme/dfperf'
summ=json.load(open(R+'/runs/baseline/logpass/df_summary.json'))
med=lambda x: st.median(x) if x else float('nan')
def async_lines(label,arm,q):
    cont=[];aw=[]
    for d in sorted(glob.glob(f'{R}/runs/op1a/{label}/{arm}/tpch_*_q{q}')):
        for f in glob.glob(d+'/log_dir/sirius_*.log'):
            for l in open(f,errors='replace'):
                if '[df_async] continuation' in l:
                    kv=dict(re.findall(r'(\w+)=(-?[\d.]+)',l)); cont.append({k:float(v) for k,v in kv.items()})
                elif '[df_async] await' in l:
                    kv=dict(re.findall(r'(\w+)=(-?[\d.]+)',l)); aw.append({k:float(v) for k,v in kv.items()})
    return cont,aw
def analyze(label,arm,q):
    cont,aw=async_lines(label,arm,q); rows=collections.defaultdict(lambda: collections.defaultdict(list)); walls=[]
    joins=[j for j in summ[str(q)]['joins'] if j['started']]
    for sess in sorted(glob.glob(f'{R}/telemetry/op1a_{label}/{arm}/q{q}/*/')):
        s=qload.load(sess); qs=list(s['queries'].items())
        for qi,(qid,qev) in enumerate(qs):
            if qi<2 or 'Finished' not in qev: continue
            t0=qev['Executing']; t1=qev['Finished']; walls.append((t1-t0)/1e6)
            plans=[p for p,qq in s['plans'].items() if qq==qid]
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
                    elif k=='Preparing': d.setdefault('preparing',t)
                    elif k=='Computing': d.setdefault('c0',t)
                    elif k=='Finalizing': d['end']=t
                if 'end' in d and 'created' in d: T[pid].append(d)
            def find(pat): return [i for i,o in ops.items() if re.search(pat,o['label'])]
            def anc_scans(pid,probe_only,seen=None):
                seen=seen if seen is not None else set(); res=[]
                for src,lb in inc.get(pid,[]):
                    if probe_only and lb=='build_receiver': continue
                    if src in seen: continue
                    seen.add(src)
                    if 'GPU_SCAN' in ops[src]['label']: res.append(src)
                    res+=anc_scans(src,probe_only,seen)
                return res
            for j in joins:
                J=j['join']; jp=find(r'HASH_JOIN\(%d\)'%J) or find(r'\(%d\)'%J)
                if not jp: continue
                jp=jp[0]; b=[src for src,lb in inc[jp] if lb=='build_receiver']
                chain=[]; cur=b[0] if b else None
                while cur is not None and len(chain)<6:
                    chain.append(cur)
                    if 'BUILD_FILTER' in ops[cur]['label'] or 'GPU_SCAN' in ops[cur]['label']: break
                    nx=[src for src,lb in inc.get(cur,[])]; cur=nx[0] if len(nx)==1 else None
                bp=[c for c in chain if re.match(r'PARTITION',ops[c]['label'])]
                bf=[c for c in chain if 'BUILD_FILTER' in ops[c]['label']]
                bc=[c for c in chain if re.match(r'CONCAT',ops[c]['label'])]
                site=(bf or bp or [None])[0]
                if site is None or not T[site]: continue
                r=rows[J]; ts=sorted(T[site],key=lambda d:d['end']); last=ts[-1]
                r['site_label']=ops[site]['label']; r['kinds']='+'.join(k['kind'] for k in j['keys'])
                r['bloom_mb']=sum(int(k['size']) for k in j['keys'] if k['kind']=='bloom')/2**20
                r['pubtask'].append((last['end']-last.get('c0',last['created']))/1e6)
                r['site'].append((last['end']-min(d['created'] for d in ts))/1e6)
                if bc and T[bc[0]] and not bf:
                    cs=T[bc[0]]; r['concat'].append((max(d['end'] for d in cs)-min(d['created'] for d in cs))/1e6)
                    r['site_to_concat_end'].append((max(d['end'] for d in cs)-min(d['created'] for d in ts))/1e6)
                # publication time
                c=[x for x in cont if int(x['join'])==J and t0<=x['handoff_epoch_ns']<=t1]
                if c:
                    tpub=c[0]['visible_epoch_ns'] if c[0]['visible_epoch_ns']>0 else c[0]['end_epoch_ns']
                    r['cont'].append(c[0]['run_ms']); r['start_delay_us'].append(c[0]['start_delay_us'])
                    r['pub_minus_handoff'].append((tpub-c[0]['handoff_epoch_ns'])/1e6)
                    a=[x['waited_ms'] for x in aw if int(x['join'])==J and t0<=x['released_epoch_ns']<=t1]
                    r['await'].append(max(a) if a else 0.0)
                else:
                    tpub=last['end']
                bsub=set(anc_scans(b[0],False))|{b[0]} if b else set()
                for sc in anc_scans(jp,False):
                    if sc in bsub or 'DYNAMIC_FILTER' not in ops[sc]['label'] or not T[sc]: continue
                    r['gapC'].append((min(d['created'] for d in T[sc])-tpub)/1e6)
                    r['gapP'].append((min(d.get('preparing',d['created']) for d in T[sc])-tpub)/1e6)
                    r['early'].append(len([d for d in T[sc] if d['created']<tpub]))
                    r['nscan'].append(1)
    return rows,walls
if __name__=='__main__':
    label=sys.argv[1]; arms=sys.argv[2].split(','); qs=[int(x) for x in sys.argv[3:]] or [q for q in range(1,23)]
    print('| q | arm | join | site | kinds | bloom MiB | n | pubtask | site | concat | site->concat end | cont | start delay us | await | min gapC | min gapP | early scans | wall |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for q in qs:
        if str(q) not in summ or not any(j['started'] for j in summ[str(q)]['joins']): continue
        for arm in arms:
            rows=collections.defaultdict(lambda: collections.defaultdict(list)); walls=[]
            for a in arm.split('+'):
                rr,ww=analyze(label,a,q); walls+=ww
                for J,r in rr.items():
                    for k,v in r.items():
                        if isinstance(v,list): rows[J][k]+=v
                        else: rows[J][k]=v
            for J,r in rows.items():
                f=lambda k: ('%.1f'%med(r[k])) if r[k] else '-'
                print(f"| {q} | {arm} | {J} | {r['site_label']} | {r['kinds']} | {r['bloom_mb']:.0f} | {len(r['pubtask'])} | {f('pubtask')} | {f('site')} | {f('concat')} | {f('site_to_concat_end')} | {f('cont')} | {f('start_delay_us')} | {f('await')} | {min(r['gapC']) if r['gapC'] else float('nan'):.2f} | {min(r['gapP']) if r['gapP'] else float('nan'):.2f} | {sum(r['early'])} of {len(r['early'])} scan-iters | {med(walls):.0f} |")
