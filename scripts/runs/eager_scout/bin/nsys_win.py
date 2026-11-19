#!/usr/bin/env python3
# nsys_win.py <sqlite> <window_index> <lo_ms> <hi_ms>: memcpy records grouped by kind/src/dst within [lo,hi] of the query window (read-only)
import sqlite3,sys,collections
path=sys.argv[1]; wi=int(sys.argv[2]); lo=float(sys.argv[3]); hi=float(sys.argv[4])
c=sqlite3.connect(f'file:{path}?mode=ro&immutable=1',uri=True).cursor()
S=dict(c.execute('select id,value from StringIds'))
qw=sorted((s,e) for t,tid,s,e in c.execute('select text,textId,start,end from NVTX_EVENTS where end is not null') if (S.get(tid) if t is None else t)=='sirius::query')
s0,_=qw[wi]; a=s0+lo*1e6; b=s0+hi*1e6
cols=[r[1] for r in c.execute('pragma table_info(CUPTI_ACTIVITY_KIND_MEMCPY)')]
print(cols)
def union(iv):
    iv=sorted(iv); tot=0; cs=ce=None
    for x,y in iv:
        if ce is None or x>ce:
            if ce is not None: tot+=ce-cs
            cs,ce=x,y
        else: ce=max(ce,y)
    return tot+(ce-cs if ce is not None else 0)
g=collections.defaultdict(list)
for s,e,d,by,k,sk,dk,sd,dd in c.execute('select start,end,deviceId,bytes,copyKind,srcKind,dstKind,srcDeviceId,dstDeviceId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and start<?',(a,b)):
    g[(k,sk,dk,d,sd,dd)].append((s,e,by))
print('copyKind srcKind dstKind dev srcDev dstDev | n  GB  union_ms  sum_ms  GB/s(union)  med MB')
for key,v in sorted(g.items()):
    tb=sum(x[2] for x in v); u=union([(x[0],x[1]) for x in v]); sm=sum(x[1]-x[0] for x in v)
    if tb<5e7: continue
    szs=sorted(x[2] for x in v)
    print(key,'| %5d %6.2f %7.1f %7.1f %6.1f %7.1f'%(len(v),tb/1e9,u/1e6,sm/1e6,tb/1e9/(u/1e9) if u else 0,szs[len(szs)//2]/1e6))
