import sqlite3,csv,sys
B='/home/ubuntu/sirius-scaling-20261105/'
def run(d):
    rows=[r for r in csv.DictReader(open(B+d+'/nsys/map.csv')) if r['iter']=='2']
    tot={}
    for r in rows:
        c=sqlite3.connect(r['sqlite'])
        s,e=int(r['range_start_ns']),int(r['range_end_ns'])
        q='''select s.value, count(*), sum(r.end-r.start) from CUPTI_ACTIVITY_KIND_RUNTIME r join StringIds s on r.nameId=s.id
             where r.start>=? and r.end<=? and (s.value like 'cudaMallocFromPoolAsync%' or s.value like 'cudaFreeAsync%') group by s.value'''
        for name,n,t in c.execute(q,(s,e)):
            k=(name.split('_')[0]); a=tot.setdefault(k,[0,0]); a[0]+=n; a[1]+=t
            print(d.split('/')[-1],r['query'],k,n,'%.1f us'%(t/n/1e3),file=sys.stderr)
    for k,(n,t) in tot.items(): print(d,k,n,'thread-s %.2f mean %.1f us'%(t/1e9,t/n/1e3))
for d in sys.argv[1:]: run(d)
