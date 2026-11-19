#!/usr/bin/env python3
"""nsys_bytes.py <nsys.sqlite> : whole-window accounting per hot sirius::query window (independent of DF ranges).
Per window: memcpy bytes/count/busy-union by kind and by (src,dst) device; memset bytes; kernel GPU-ms per device for Bloom insert / probe;
NVTX dynfilter::* range walls."""
import sqlite3,sys,collections,re
KIND={1:'HtoD',2:'DtoH',8:'DtoD',10:'PtoP'}
def union(iv):
    iv=sorted(iv); tot=0; cs=ce=None
    for a,b in iv:
        if ce is None or a>ce:
            if ce is not None: tot+=ce-cs
            cs,ce=a,b
        else: ce=max(ce,b)
    return tot+(ce-cs if ce is not None else 0)
c=sqlite3.connect(sys.argv[1]).cursor()
S=dict(c.execute('select id,value from StringIds'))
nv=[(S.get(tid) if t is None else t,s,e) for t,tid,s,e in c.execute('select text,textId,start,end from NVTX_EVENTS')]
qw=sorted((s,e) for n,s,e in nv if n=='sirius::query' and e is not None)
mc=list(c.execute('select start,end,deviceId,bytes,copyKind,srcDeviceId,dstDeviceId from CUPTI_ACTIVITY_KIND_MEMCPY'))
ms=list(c.execute('select start,end,deviceId,bytes from CUPTI_ACTIVITY_KIND_MEMSET'))
kn=list(c.execute('select start,end,deviceId,demangledName,shortName from CUPTI_ACTIVITY_KIND_KERNEL'))
for wi,(ws,we) in enumerate(qw):
    print('== window %d wall %.1f ms'%(wi,(we-ws)/1e6))
    agg=collections.defaultdict(lambda:[0,0,[]])
    for s,e,d,b,k,sd,dd in mc:
        if s<ws or s>=we: continue
        a=agg[KIND.get(k,str(k))]; a[0]+=1; a[1]+=b; a[2].append((s,e))
    for k,a in sorted(agg.items()): print('  memcpy %-5s n=%6d  %9.1f MB  busy-union %7.1f ms'%(k,a[0],a[1]/1e6,union(a[2])/1e6))
    tb=sum(b for s,e,d,b in ms if ws<=s<we); print('  memset %.1f MB'%(tb/1e6))
    ka=collections.defaultdict(lambda:collections.defaultdict(float)); kc=collections.Counter()
    for s,e,d,dn,sn in kn:
        if s<ws or s>=we: continue
        n=S.get(dn) or S.get(sn) or '?'
        if 'add_if' in n or 'bloom' in n.lower() and 'add' in n: key='bloom_insert'
        elif 'or_bloom_chunk' in n: key='or_bloom_chunk'
        elif 'membership_probe' in n or 'bloom_lookup' in n: key='df_probe'
        else: key='other'
        ka[key][d]+=(e-s)/1e6; kc[key]+=1
    for key in ('bloom_insert','or_bloom_chunk','df_probe','other'):
        if key in ka: print('  kernels %-14s n=%6d  per-device ms: %s  sum %.1f'%(key,kc[key],' '.join('%d:%.1f'%(d,v) for d,v in sorted(ka[key].items())),sum(ka[key].values())))
    r=collections.defaultdict(lambda:[0,0.0])
    for n,s,e in nv:
        if n and n.startswith('dynfilter') and e is not None and ws<=s<we: r[n][0]+=1; r[n][1]+=(e-s)/1e6
    for n,a in sorted(r.items()): print('  nvtx %-45s n=%4d  sum %.2f ms'%(n,a[0],a[1]))
