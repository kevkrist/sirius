#!/usr/bin/env python3
"""nsys_df.py <nsys.sqlite> <out.json>: DF attribution per hot iteration (sirius::query windows 2..N).
Same-thread attribution: a CUDA API call belongs to a range if it is issued on the range's thread inside it; kernels/memcpy/memset follow by correlationId."""
import sqlite3,sys,json,bisect,collections,re
KIND={1:'HtoD',2:'DtoH',8:'DtoD',10:'PtoP'}
def union(iv):
    iv=sorted(iv); tot=0; cs=ce=None
    for a,b in iv:
        if ce is None or a>ce:
            if ce is not None: tot+=ce-cs
            cs,ce=a,b
        else: ce=max(ce,b)
    return tot+(ce-cs if ce is not None else 0)
def short(n):
    n=re.sub(r'<.*','',n or '?'); return n[-60:]
def main(path,outp):
    c=sqlite3.connect(path).cursor()
    S=dict(c.execute('select id,value from StringIds'))
    nv=[(S.get(tid) if t is None else t,s,e,g,et) for t,tid,s,e,g,et in c.execute('select text,textId,start,end,globalTid,eventType from NVTX_EVENTS')]
    rng=[(n,s,e,g) for n,s,e,g,et in nv if e is not None and n]
    marks=[(n,s,g) for n,s,e,g,et in nv if e is None and n and n.startswith('dynfilter')]
    qw=sorted((s,e) for n,s,e,g in rng if n=='sirius::query')
    rt=collections.defaultdict(list)
    for s,e,g,corr,nid in c.execute('select start,end,globalTid,correlationId,nameId from CUPTI_ACTIVITY_KIND_RUNTIME order by start'):
        rt[g].append((s,e,corr,S.get(nid,'?').split('_v')[0]))
    rts={g:[x[0] for x in v] for g,v in rt.items()}
    K={corr:(s,e,d,st,short(S.get(dn) or S.get(sn))) for s,e,d,st,corr,dn,sn in c.execute('select start,end,deviceId,streamId,correlationId,demangledName,shortName from CUPTI_ACTIVITY_KIND_KERNEL')}
    M=collections.defaultdict(list)
    for s,e,d,b,k,sd,dd,corr in c.execute('select start,end,deviceId,bytes,copyKind,srcDeviceId,dstDeviceId,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY'): M[corr].append((s,e,b,KIND.get(k,str(k)),sd,dd))
    MS=collections.defaultdict(list)
    for s,e,b,corr in c.execute('select start,end,bytes,correlationId from CUPTI_ACTIVITY_KIND_MEMSET'): MS[corr].append((s,e,b))
    def calls(s,e,g):
        v=rt.get(g,[]); i=bisect.bisect_left(rts.get(g,[]),s); out=[]
        while i<len(v) and v[i][0]<e:
            if v[i][1]<=e: out.append(v[i])
            i+=1
        return out
    def stats(s,e,g):
        cs=calls(s,e,g); d=dict(wall=(e-s)/1e6,n_api=len(cs),api=collections.Counter(),api_ms=collections.Counter(),kern=collections.Counter(),kern_n=collections.Counter(),
                                 mc=collections.defaultdict(lambda:[0,0,0.0]),memset=[0,0,0.0],sync_ms=0.0,n_sync=0)
        kiv=[];hiv=[];piv=[]
        for cs_,ce_,corr,name in cs:
            d['api'][name]+=1; d['api_ms'][name]+=(ce_-cs_)/1e6
            if 'Synchronize' in name: d['sync_ms']+=(ce_-cs_)/1e6; d['n_sync']+=1
            if corr in K: k=K[corr]; d['kern'][k[4]]+=(k[1]-k[0])/1e6; d['kern_n'][k[4]]+=1; kiv.append((k[0],k[1]))
            for m in M.get(corr,[]):
                a=d['mc'][m[3]]; a[0]+=1; a[1]+=m[2]; a[2]+=(m[1]-m[0])/1e6
                (hiv if m[3] in('HtoD','DtoH') else piv).append((m[0],m[1]))
            for m in MS.get(corr,[]): d['memset'][0]+=1; d['memset'][1]+=m[2]; d['memset'][2]+=(m[1]-m[0])/1e6
        d['kern_union']=union(kiv)/1e6; d['host_copy_union']=union(hiv)/1e6; d['other_copy_union']=union(piv)/1e6
        d['gpu_last_end']=max([x[1] for x in kiv+hiv+piv],default=None)
        for k in('api','api_ms','kern','kern_n'): d[k]=dict(d[k])
        d['mc']={k:v for k,v in d['mc'].items()}
        return d
    DF=[r for r in rng if r[0].startswith('dynfilter::') and not r[0].startswith('dynfilter::apply') and 'membership' not in r[0]]
    PE=[r for r in rng if r[0] in('sirius_physical_partition::execute','sirius_physical_build_filter::execute')]
    res=[]
    for wi,(ws,we) in enumerate(qw):
        if wi==0: continue
        it=dict(iter=wi,wall=(we-ws)/1e6,ranges={},sites=[])
        dfw=[r for r in DF if ws<=r[1]<we]
        # top-level DF ranges (not nested in another DF range on the same thread)
        top=[r for r in dfw if not any(o is not r and o[3]==r[3] and o[1]<=r[1] and o[2]>=r[2] for o in dfw)]
        it['df_top_wall']=sum(r[2]-r[1] for r in top)/1e6
        it['df_top_union']=union([(r[1],r[2]) for r in top])/1e6
        byn=collections.defaultdict(list)
        for r in dfw: byn[r[0]].append(r)
        for n,L in byn.items():
            agg=None
            for r in L:
                d=stats(r[1],r[2],r[3])
                if agg is None: agg=d; agg['n']=1; agg['max_wall']=d['wall']; continue
                agg['n']+=1; agg['max_wall']=max(agg['max_wall'],d['wall'])
                for k in('wall','n_api','sync_ms','n_sync','kern_union','host_copy_union','other_copy_union'): agg[k]+=d[k]
                for k in('api','api_ms','kern','kern_n'):
                    for a,b in d[k].items(): agg[k][a]=agg[k].get(a,0)+b
                for a,b in d['mc'].items():
                    x=agg['mc'].setdefault(a,[0,0,0.0]); x[0]+=b[0];x[1]+=b[1];x[2]+=b[2]
                for i in range(3): agg['memset'][i]+=d['memset'][i]
            agg.pop('gpu_last_end',None); it['ranges'][n]=agg
        # marks
        it['marks']=collections.Counter(m[0] for m in marks if ws<=m[1]<we)
        # sites: partition/build_filter execute ranges that contain DF ranges
        allpe=[r for r in PE if ws<=r[1]<we]
        it['partition_execute_all_ms']=sum(r[2]-r[1] for r in allpe if 'partition' in r[0])/1e6; it['partition_execute_n']=len([r for r in allpe if 'partition' in r[0]])
        for r in allpe:
            inner=[o for o in dfw if o[3]==r[3] and o[1]>=r[1] and o[2]<=r[2]]
            if not inner: continue
            itop=[o for o in inner if not any(p is not o and p[1]<=o[1] and p[2]>=o[2] for p in inner)]
            d=stats(r[1],r[2],r[3])
            enc=[o for o in rng if o[3]==r[3] and o[1]<=r[1] and o[2]>=r[2] and re.match(r'Pipeline \d+: ',o[0])]
            enc=min(enc,key=lambda o:o[2]-o[1]) if enc else None
            names=collections.Counter(o[0].replace('dynfilter::','') for o in inner)
            fin=[o for o in inner if o[0] in('dynfilter::accum::finish','dynfilter::stage::gather','dynfilter::bounds::finish','dynfilter::replicate_devices')]
            con=[o for o in inner if o[0].endswith('::contribute')]
            site=dict(task=(enc[0] if enc else None),task_wall=((enc[2]-enc[1])/1e6 if enc else None),op=r[0].split('::')[0].replace('sirius_physical_',''),start=(r[1]-ws)/1e6,tid=r[3],wall=(r[2]-r[1])/1e6,df_wall=sum(o[2]-o[1] for o in itop)/1e6,names=dict(names),
                      contribute_wall=sum(o[2]-o[1] for o in con)/1e6,finish_wall=sum(o[2]-o[1] for o in fin if o in itop)/1e6,
                      publishing=bool(fin),kern={k:round(v,3) for k,v in d['kern'].items()},kern_union=d['kern_union'],sync_ms=d['sync_ms'],n_sync=d['n_sync'],n_api=d['n_api'],
                      host_copy_union=d['host_copy_union'],mc=d['mc'])
            if con:
                cd=[stats(o[1],o[2],o[3]) for o in con]
                site['contribute_kern']={}; 
                for x in cd:
                    for k,v in x['kern'].items(): site['contribute_kern'][k]=site['contribute_kern'].get(k,0)+v
                site['contribute_n_api']=sum(x['n_api'] for x in cd)
                # H04: do the contribute kernels finish before the range's last GPU activity, and what follows on the task
                ce=max(o[2] for o in con); ke=max([x['gpu_last_end'] for x in cd if x['gpu_last_end']],default=None)
                site['contrib_kernels_end_after_contribute_return_ms']=None if ke is None else (ke-ce)/1e6
                site['after_contribute_to_first_finish_ms']=((min(o[1] for o in fin)-ce)/1e6) if fin else (r[2]-ce)/1e6
            if fin:
                site['pre_finish_ms']=(min(o[1] for o in fin)-r[1])/1e6; site['post_finish_ms']=(r[2]-max(o[2] for o in fin))/1e6
            it['sites'].append(site)
        # DF ranges outside any execute range (begin on creator thread etc.)
        res.append(it)
    json.dump(res,open(outp,'w'),indent=1,default=dict)
main(*sys.argv[1:3])
