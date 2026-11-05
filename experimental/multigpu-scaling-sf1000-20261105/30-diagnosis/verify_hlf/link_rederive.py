# Independent re-derivation of per-query host->device bytes and per-switch link-busy (union) time, nsys iter2 windows.
import csv,sqlite3,sys,collections
base='/home/ubuntu/sirius-scaling-20261105/01-nsys/'
def union(iv):
    iv.sort(); tot=0; cs=ce=None
    for s,e in iv:
        if cs is None: cs,ce=s,e
        elif s>ce: tot+=ce-cs; cs,ce=s,e
        else: ce=max(ce,e)
    if cs is not None: tot+=ce-cs
    return tot
out=[]
for run in ['nsys2_g1','nsys2_g4']:
    for r in csv.DictReader(open(base+run+'/nsys/map.csv')):
        if r['iter']!='2': continue
        q=int(r['query']); s0=int(r['range_start_ns']); s1=int(r['range_end_ns'])
        c=sqlite3.connect(r['sqlite'])
        rows=c.execute("select start,end,deviceId,bytes,srcKind,dstKind,copyKind from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and start<?",(s0,s1)).fetchall()
        h2d=collections.Counter(); d2h=0; iv={0:[],1:[]}; big={0:[],1:[]}; d2d=0
        for st,en,dev,b,sk,dk,ck in rows:
            if sk in (0,1) and dk==2:
                h2d[dev]+=b; sw=0 if dev in (0,1) else 1; iv[sw].append((st,min(en,s1)))
            elif sk==2 and dk in (0,1): d2h+=b
            elif sk==2 and dk==2: d2d+=b
        wall=(s1-s0)/1e9
        H=sum(h2d.values())
        sw=[h2d[0]+h2d[1],h2d[2]+h2d[3]]
        bu=[union(iv[0])/1e9,union(iv[1])/1e9]
        out.append(dict(run=run,q=q,wall=round(wall,4),h2d_GB=round(H/1e9,2),d2h_GB=round(d2h/1e9,2),scan_est_GB=round((H-d2h)/1e9,2),
            sw0_GB=round(sw[0]/1e9,2),sw1_GB=round(sw[1]/1e9,2),busy0=round(bu[0],4),busy1=round(bu[1],4)))
w=csv.DictWriter(open(sys.argv[1],'w'),out[0].keys()); w.writeheader(); [w.writerow(o) for o in out]
