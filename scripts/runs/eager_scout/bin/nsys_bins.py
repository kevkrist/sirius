#!/usr/bin/env python3
# nsys_bins.py <sqlite> <window_index> [bin_ms=20]: per-bin, per-device HtoD/DtoH/PtoP GB, copy-busy fraction, kernel-busy fraction (read-only)
import sqlite3,sys,collections
path=sys.argv[1]; wi=int(sys.argv[2]); B=float(sys.argv[3]) if len(sys.argv)>3 else 20.0
c=sqlite3.connect(f'file:{path}?mode=ro&immutable=1',uri=True).cursor()
S=dict(c.execute('select id,value from StringIds'))
qw=sorted((s,e) for t,tid,s,e in c.execute('select text,textId,start,end from NVTX_EVENTS where end is not null') if (S.get(tid) if t is None else t)=='sirius::query')
print('query windows ms:',[round((e-s)/1e6,1) for s,e in qw])
s0,e0=qw[wi]
KIND={1:'HtoD',2:'DtoH',8:'DtoD',10:'PtoP'}
nb=int((e0-s0)/1e6/B)+1
by=collections.defaultdict(lambda: collections.defaultdict(float))   # (bin,dev)->kind->bytes
busy=collections.defaultdict(lambda: collections.defaultdict(float)) # (bin,dev)->'copy'/'kern' ns (sum, may exceed bin if concurrent)
def spread(s,e,key,sub,val_total=None):
    s=max(s,s0); e=min(e,e0)
    if e<=s: return
    b0=int((s-s0)/1e6/B); b1=int((e-s0)/1e6/B)
    for b in range(b0,b1+1):
        lo=s0+b*B*1e6; hi=lo+B*1e6
        ov=min(e,hi)-max(s,lo)
        if ov<=0: continue
        if val_total is None: busy[(b,key)][sub]+=ov
        else: by[(b,key)][sub]+=val_total*ov/(e-s)
for s,e,d,b,k,sd,dd in c.execute('select start,end,deviceId,bytes,copyKind,srcDeviceId,dstDeviceId from CUPTI_ACTIVITY_KIND_MEMCPY where end>? and start<?',(s0,e0)):
    kind=KIND.get(k,str(k))
    dev = dd if kind=='PtoP' else d
    spread(s,max(e,s+1),dev,kind,b)
    spread(s,max(e,s+1),dev,'copy_'+kind)
for s,e,d in c.execute('select start,end,deviceId from CUPTI_ACTIVITY_KIND_KERNEL where end>? and start<?',(s0,e0)):
    spread(s,max(e,s+1),d,'kern')
devs=sorted({k[1] for k in list(by)+list(busy)})
print('bin_ms | per device: HtoD GB/s, DtoH GB/s, PtoP-in GB/s, kern-busy(sum ms/ms)')
for b in range(nb):
    row=[]
    for d in devs:
        x=by[(b,d)]; y=busy[(b,d)]
        row.append('%5.1f %5.1f %5.1f k%4.2f'%(x['HtoD']/1e9/(B/1e3),x['DtoH']/1e9/(B/1e3),x['PtoP']/1e9/(B/1e3),y['kern']/1e6/B))
    print('%6.0f | '%(b*B)+' | '.join(row))
