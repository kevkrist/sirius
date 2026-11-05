"""GPU-origin prepare (clone) thread-ms per query/pipeline label, hot iters 2-4 averaged.
usage: gprep.py <quent_dir> [iters=2,3,4]"""
import csv, sys, collections
d = sys.argv[1]; iters = (sys.argv[2] if len(sys.argv) > 2 else '2,3,4').split(',')
labels = {r['pipeline'] if 'pipeline' in r else r.get('uuid'): r for r in csv.DictReader(open(f'{d}/pipelines.csv'))}
T = collections.defaultdict(float); N = collections.Counter(); HT = collections.defaultdict(float)
maxprep = collections.defaultdict(float)
for r in csv.DictReader(open(f'{d}/tasks.csv')):
    q, it = r['query'].rsplit('_iter', 1)
    if it not in iters or not r['first_compute'] or not r['preparing']: continue
    p = (int(r['first_compute']) - int(r['preparing'])) / 1e6
    if r['origin'] == 'GPU':
        T[q] += p; N[q] += 1
    else:
        HT[q] += p
k = len(iters)
tot = 0
for q in sorted(T, key=lambda s: int(s.split('q')[1])):
    print(f'{q:10} gpuprep_thread_ms {T[q]/k:9.1f}  ntasks {N[q]/k:6.1f}')
    tot += T[q]/k
print('TOTAL gpu-origin prepare thread-s per hot iter', round(tot/1e3, 2), ' host-origin', round(sum(HT.values())/k/1e3, 2))
