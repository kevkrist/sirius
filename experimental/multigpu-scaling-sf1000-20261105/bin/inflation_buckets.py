#!/usr/bin/env python3
"""inflation_buckets.py <quentA> <quentB> [iter=iter3]: per query, summed task time split into buckets
(scan_prep, nonscan_prep, partition_op, scan_ops, other_ops); prints A sum, B sum (total over all GPUs)."""
import csv,sys,collections
it=sys.argv[3] if len(sys.argv)>3 else 'iter3'
def load(d):
    pl={r['pipeline']:(r['query'],r['label']) for r in csv.DictReader(open(d+'/pipelines.csv')) if r['query'] and r['query'].endswith(it)}
    B=collections.defaultdict(collections.Counter); pot={}
    for r in csv.DictReader(open(d+'/tasks.csv')):
        pot[r['task']]=r['pipeline']
        if r['pipeline'] in pl and r['first_compute'] and r['preparing']:
            q,l=pl[r['pipeline']]; k='scan_prep' if l.startswith('GPU_SCAN') else 'nonscan_prep'
            B[q][k]+=(int(r['first_compute'])-int(r['preparing']))/1e6
    oplabel={}
    for r in csv.DictReader(open(d+'/task_ops.csv')):
        p=pot.get(r['task'])
        if p in pl and r['end']:
            q,l=pl[p]; ms=(int(r['end'])-int(r['start']))/1e6
            # find operator name from label by id
            name=next((s.split('(')[0] for s in l.split(' -> ') if s.endswith(f"({r['operator_id']})")),'?')
            k='partition_op' if name=='PARTITION' else ('scan_op' if name=='GPU_SCAN' else ('concat_op' if name=='CONCAT' else 'other_ops'))
            B[q][k]+=ms
    return B
A=load(sys.argv[1]); Bb=load(sys.argv[2])
keys=['scan_prep','scan_op','nonscan_prep','partition_op','concat_op','other_ops']
print(f"{'query':16}"+''.join(f"{k:>22}" for k in keys))
tot=collections.defaultdict(lambda:[0,0])
for q in sorted(A,key=lambda s:int(s.split('_q')[1].split('_')[0])):
    row=f"{q[5:]:16}"
    for k in keys:
        a,b=A[q][k],Bb.get(q,{}).get(k,0); tot[k][0]+=a; tot[k][1]+=b
        row+=f"{a:9.0f}->{b:7.0f}({(b/a if a>1 else 0):4.1f})"
    print(row)
print(f"{'TOTAL ms':16}"+''.join(f"{tot[k][0]:9.0f}->{tot[k][1]:7.0f}({tot[k][1]/max(tot[k][0],1):4.1f})" for k in keys))
