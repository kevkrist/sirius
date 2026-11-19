import sqlite3,sys,collections
N='/opt/dlami/nvme/dfperf/mblife/nsys/'
def counts(f):
    c=sqlite3.connect(N+f+'.sqlite')
    r=c.execute("select s.value,count(*),sum(r.end-r.start) from CUPTI_ACTIVITY_KIND_RUNTIME r join StringIds s on s.id=r.nameId group by 1").fetchall()
    d={n.split('_v')[0]:(k,t) for n,k,t in r}
    try:
        k=c.execute("select count(*) from CUPTI_ACTIVITY_KIND_KERNEL").fetchone()[0]; d['#kernels']=(k,0)
        m=c.execute("select count(*) from CUPTI_ACTIVITY_KIND_MEMCPY").fetchone()[0]; d['#memcpy_act']=(m,0)
        m=c.execute("select count(*) from CUPTI_ACTIVITY_KIND_MEMSET").fetchone()[0]; d['#memset_act']=(m,0)
    except Exception as e: pass
    return d
for cfg in sys.argv[1:]:
    a={(B,r):counts(f'ns_{cfg}_b{B}_r{r}') for B in (1,512) for r in (1,5)}
    names=sorted(set().union(*[set(x) for x in a.values()]))
    print(f"== {cfg}: per-attempt fixed (begin+1 contribution+publish, B=1) | per extra contribution ; [us per call]")
    for n in names:
        g=lambda B,r,i: a[(B,r)].get(n,(0,0))[i]
        fixed=(g(1,5,0)-g(1,1,0))/4; per=((g(512,5,0)-g(512,1,0))/4-fixed)/511
        tot=g(512,5,0); us=g(512,5,1)/tot/1e3 if tot else 0
        if abs(fixed)>0.01 or abs(per)>0.01: print(f"  {n:38s} {fixed:7.2f} | {per:6.2f}  [{us:.2f}]")
