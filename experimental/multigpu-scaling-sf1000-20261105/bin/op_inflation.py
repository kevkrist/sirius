#!/usr/bin/env python3
"""op_inflation.py <runA_quent> <runB_quent> <query label suffix e.g. q18_iter3>
Per pipeline (label) and per operator: summed task-ms in A vs B, prepare-ms (preparing->first compute) A vs B."""
import csv,sys,collections
def load(d,q):
    pl={r['pipeline']:r['label'] for r in csv.DictReader(open(d+'/pipelines.csv')) if r['query'] and r['query'].endswith(q)}
    ops=collections.Counter(); prep=collections.Counter(); n=collections.Counter(); hb=collections.Counter()
    for r in csv.DictReader(open(d+'/tasks.csv')):
        if r['pipeline'] in pl and r['first_compute'] and r['preparing']:
            k=pl[r['pipeline']]; prep[k]+=(int(r['first_compute'])-int(r['preparing']))/1e6; n[k]+=1
    pipe_of_task={}
    for r in csv.DictReader(open(d+'/tasks.csv')): pipe_of_task[r['task']]=r['pipeline']
    for r in csv.DictReader(open(d+'/task_ops.csv')):
        p=pipe_of_task.get(r['task'])
        if p in pl and r['end']: ops[(pl[p],r['operator_id'])]+=(int(r['end'])-int(r['start']))/1e6
    return ops,prep,n
q=sys.argv[3]; A=load(sys.argv[1],q); B=load(sys.argv[2],q)
print(f"{'pipeline':70} {'nA':>5} {'nB':>5} {'prepA':>8} {'prepB':>8}")
for k in sorted(set(A[2])|set(B[2]), key=lambda k:-(B[1][k])):
    print(f"{k[:70]:70} {A[2][k]:5} {B[2][k]:5} {A[1][k]:8.0f} {B[1][k]:8.0f}")
print(f"\n{'pipeline/op':78} {'opA':>8} {'opB':>8} {'B/A':>5}")
for k in sorted(set(A[0])|set(B[0]), key=lambda k:-B[0][k])[:25]:
    a,b=A[0][k],B[0][k]; print(f"{(k[0][:70]+' op'+k[1]):78} {a:8.0f} {b:8.0f} {b/a if a else 0:5.1f}")
print(f"TOTAL prep A={sum(A[1].values()):.0f} B={sum(B[1].values()):.0f}  ops A={sum(A[0].values()):.0f} B={sum(B[0].values()):.0f}")
