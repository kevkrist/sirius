"""Wall attribution of GPU-origin prepare (clone) per query, hot iters averaged.
f(t) = n_gpuprep(t) / n_active(t)  (tasks active = preparing..finalize, all GPUs)
attrib = integral f dt  (ms)   ; pure = time where n_active>0 and all active tasks are in GPU-prep
Also wall share where every active GPU is only cloning.
usage: clonewall.py <quent_dir> [iters]"""
import csv, sys, collections
d = sys.argv[1]; iters = (sys.argv[2] if len(sys.argv) > 2 else '2,3,4').split(',')
ev = collections.defaultdict(list)
for r in csv.DictReader(open(f'{d}/tasks.csv')):
    q, it = r['query'].rsplit('_iter', 1)
    if it not in iters or not r['first_compute'] or not r['preparing'] or not r['finalize']: continue
    s, fc, e = int(r['preparing']), int(r['first_compute']), int(r['finalize'])
    key = (q, it)
    ev[key].append((s, 0, +1)); ev[key].append((e, 0, -1))
    if r['origin'] == 'GPU' and fc > s:
        ev[key].append((s, 1, +1)); ev[key].append((fc, 1, -1))
A = collections.defaultdict(float); P = collections.defaultdict(float)
for key, L in ev.items():
    L.sort(); na = ng = 0; last = None
    for t, kind, dlt in L:
        if last is not None and na > 0:
            dt = (t - last) / 1e6
            A[key[0]] += dt * ng / na
            if ng == na: P[key[0]] += dt
        if kind == 0: na += dlt
        else: ng += dlt
        last = t
k = len(iters); ta = tp = 0
for q in sorted(A, key=lambda s: int(s.split('q')[1])):
    print(f'{q:10} attrib_ms {A[q]/k:8.1f} pure_ms {P[q]/k:8.1f}'); ta += A[q]/k; tp += P[q]/k
print(f'TOTAL attrib_s {ta/1e3:.3f} pure_s {tp/1e3:.3f}')
