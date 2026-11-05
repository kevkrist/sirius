import csv,collections,sys
B='/home/ubuntu/sirius-scaling-20261105/00-baseline/'
def load(run):
    lab={}
    for r in csv.DictReader(open(B+run+'/quent/pipelines.csv')): lab[r['pipeline']]=r['label']
    T=collections.defaultdict(float)   # (q, pipe_type) -> thread ms of PARTITION task (prep->finalize)
    O=collections.defaultdict(float)
    for r in csv.DictReader(open(B+run+'/quent/tasks.csv')):
        q,it=r['query'].rsplit('_iter',1)
        if it not in('2','3','4'): continue
        l=lab.get(r['pipeline'],'')
        if not l.startswith('PARTITION'): continue
        if not r['finalize'] or not r['preparing']: continue
        T[(q,r['pipeline_type'],l.split(' ')[0])]+=(int(r['finalize'])-int(r['preparing']))/1e6/3
    return T
g1=load('g1_a'); g4=load('g4_a')
forced4=forced1=nat4=nat1=0;rows=[]
for k in set(g1)|set(g4):
    a=g1.get(k,0); b=g4.get(k,0)
    if b>0 and a<0.1*b: forced4+=b; forced1+=a; rows.append((b,a,k))
    else: nat4+=b; nat1+=a
print('forced class PARTITION task thread-s: g4 %.2f g1 %.3f ; natural g4 %.2f g1 %.2f'%(forced4/1e3,forced1/1e3,nat4/1e3,nat1/1e3))
rows.sort(reverse=True)
for b,a,k in rows[:25]: print('%8.1f %7.1f %s'%(b,a,k))
