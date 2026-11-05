#!/usr/bin/env python3
"""x1_report.py <g1.json> <g4.json>: suite tables for the X1 allocator report (iter2 nsys windows)."""
import json, sys
from collections import defaultdict

A = json.load(open(sys.argv[1])); B = json.load(open(sys.argv[2]))
qs = sorted(set(A) & set(B), key=int)
M, F = 'cudaMallocFromPoolAsync', 'cudaFreeAsync'
SY = ('cudaStreamSynchronize', 'cuStreamSynchronize')


def tot(R, f):
    out = defaultdict(lambda: [0, 0])
    for q in qs:
        for k, v in f(R[q]).items():
            out[k][0] += v[0]; out[k][1] += v[1]
    return out


print('## 1. API totals (suite, iter2 windows)')
wa = sum(A[q]['window_ns'] for q in qs) / 1e9; wb = sum(B[q]['window_ns'] for q in qs) / 1e9
print(f'windows: g1 {wa:.2f}s g4 {wb:.2f}s')
apiA = tot(A, lambda r: r['api']); apiB = tot(B, lambda r: r['api'])
print(f"{'api':28}{'nA':>9}{'nB':>9}{'sA':>8}{'sB':>8}{'usA':>7}{'usB':>7}")
for k in sorted(apiB, key=lambda k: -apiB[k][1])[:16]:
    a = apiA.get(k, [0, 0]); b = apiB[k]
    print(f"{k[:28]:28}{a[0]:9}{b[0]:9}{a[1]/1e9:8.2f}{b[1]/1e9:8.2f}{a[1]/max(a[0],1)/1e3:7.1f}{b[1]/max(b[0],1)/1e3:7.1f}")

print('\n## 2. OSRT lock time nested in CUDA calls (s), g1 -> g4')
for R, lab in ((A, 'g1'), (B, 'g4')):
    agg = defaultdict(lambda: defaultdict(int)); cnt = defaultdict(lambda: defaultdict(int))
    for q in qs:
        for host, d in R[q]['lock'].items():
            for l, ns in d.items():
                agg[host][l] += ns
        for host, d in R[q]['lockcnt'].items():
            for l, n in d.items():
                cnt[host][l] += n
    print(lab)
    for host in sorted(agg, key=lambda h: -sum(v for l, v in agg[h].items() if l != 'futex'))[:10]:
        d = agg[host]
        print(f"  {host[:26]:26} " + ' '.join(f"{l.replace('pthread_','')}={d[l]/1e9:.2f}s/{cnt[host][l]}" for l in sorted(d)))

print('\n## 3. allocator latency vs in-flight allocator calls at call start (pooled)')
for R, lab in ((A, 'g1'), (B, 'g4')):
    c = defaultdict(lambda: [0, 0])
    for q in qs:
        for b, v in R[q]['conc'].items():
            c[b][0] += v[0]; c[b][1] += v[1]
    n = sum(v[0] for v in c.values())
    print(lab, ' '.join(f"{b}:{c[b][0]/n*100:.0f}%@{c[b][1]/max(c[b][0],1)/1e3:.1f}us" for b in ['1', '2', '3-4', '5-8', '9-12', '13+']))

print('\n## 4. per query: allocator calls, rate, mean threads in allocator, union occupancy, alloc thread-s')
print(f"{'q':>3}{'callsA':>9}{'callsB':>9}{'winA':>7}{'winB':>7}{'kcps_A':>8}{'kcps_B':>8}{'thrA':>6}{'thrB':>6}{'occA':>6}{'occB':>6}{'excess_s':>9}")
tot_ex = 0
for q in qs:
    a = A[q]; b = B[q]
    ca = sum(a['conc'][k][0] for k in a['conc']); cb = sum(b['conc'][k][0] for k in b['conc'])
    ma = a['api'].get(M, [0, 0]); fa = a['api'].get(F, [0, 0]); mb = b['api'].get(M, [0, 0]); fb = b['api'].get(F, [0, 0])
    ex = (mb[1] - mb[0] * ma[1] / max(ma[0], 1)) + (fb[1] - fb[0] * fa[1] / max(fa[0], 1))
    tot_ex += ex
    print(f"{q:>3}{ca:9}{cb:9}{a['window_ns']/1e9:7.2f}{b['window_ns']/1e9:7.2f}{ca/a['window_ns']*1e6:8.0f}{cb/b['window_ns']*1e6:8.0f}"
          f"{a['alloc_thread_ns']/a['window_ns']:6.1f}{b['alloc_thread_ns']/b['window_ns']:6.1f}{a['occ_ns']/a['window_ns']:6.2f}{b['occ_ns']/b['window_ns']:6.2f}{ex/1e9:9.2f}")
print(f'suite allocator excess thread-s at g1 per-call latency (per query, per api): {tot_ex/1e9:.2f}')

print('\n## 5. allocator calls by op-level NVTX class (suite)')
oa = defaultdict(lambda: defaultdict(lambda: [0, 0])); ob = defaultdict(lambda: defaultdict(lambda: [0, 0]))
for R, o in ((A, oa), (B, ob)):
    for q in qs:
        for op, d in R[q]['attr_op'].items():
            for n, v in d.items():
                o[op][n][0] += v[0]; o[op][n][1] += v[1]
print(f"{'op':28}{'mallocA':>9}{'mallocB':>9}{'freeA':>9}{'freeB':>9}{'sA':>7}{'sB':>7}{'usA':>6}{'usB':>6}{'excess_s':>9}")
rows = []
for op in set(oa) | set(ob):
    ma, fa = oa[op][M], oa[op][F]; mb, fb = ob[op][M], ob[op][F]
    ta = ma[1] + fa[1]; tb = mb[1] + fb[1]; na = ma[0] + fa[0]; nb = mb[0] + fb[0]
    rows.append((tb, op, ma, mb, fa, fb, ta, tb, na, nb))
gA = (apiA[M][1] + apiA[F][1]) / (apiA[M][0] + apiA[F][0])
for tb, op, ma, mb, fa, fb, ta, tb, na, nb in sorted(rows, reverse=True)[:16]:
    ex = tb - nb * (ta / na if na else gA)
    print(f"{op[:28]:28}{ma[0]:9}{mb[0]:9}{fa[0]:9}{fb[0]:9}{ta/1e9:7.2f}{tb/1e9:7.2f}{ta/max(na,1)/1e3:6.1f}{tb/max(nb,1)/1e3:6.1f}{ex/1e9:9.2f}")

print('\n## 6. allocator calls by leaf NVTX range (top 30 by g4 calls)')
la = defaultdict(lambda: [0, 0]); lb = defaultdict(lambda: [0, 0])
for R, o in ((A, la), (B, lb)):
    for q in qs:
        for leaf, d in R[q]['attr_leaf'].items():
            for n, v in d.items():
                o[leaf][0] += v[0]; o[leaf][1] += v[1]
for leaf in sorted(lb, key=lambda k: -lb[k][0])[:30]:
    a = la.get(leaf, [0, 0]); b = lb[leaf]
    print(f"  {leaf[:60]:60}{a[0]:9}{b[0]:9}{a[1]/1e9:7.2f}{b[1]/1e9:7.2f}")

print('\n## 7. allocs per task by task signature (suite; ntasks, mallocs/task, frees/task, alloc ms/task)')
ta = defaultdict(lambda: [0, 0, 0, 0]); tb_ = defaultdict(lambda: [0, 0, 0, 0])
for R, o in ((A, ta), (B, tb_)):
    for q in qs:
        for sig, v in R[q]['tasks'].items():
            key = sig.split(' -> ')[0] + '..' + sig.split(' -> ')[-1]
            for i in range(4):
                o[key][i] += v[i]
for k in sorted(tb_, key=lambda k: -tb_[k][3])[:14]:
    a = ta.get(k, [0, 0, 0, 0]); b = tb_[k]
    f = lambda v: f"{v[0]:6} {v[1]/max(v[0],1):6.1f} {v[2]/max(v[0],1):6.1f} {v[3]/max(v[0],1)/1e6:6.2f}"
    print(f"  {k[:40]:40} g1[{f(a)}]  g4[{f(b)}]")

print('\n## 8. stream syncs: class split (s) by api|op-level, g1 -> g4')
for R, lab in ((A, 'g1'), (B, 'g4')):
    s = defaultdict(lambda: defaultdict(int)); n = defaultdict(int)
    for q in qs:
        for k, d in R[q]['sync'].items():
            for c, v in d.items():
                s[k][c] += v
        for k, v in R[q]['syncn'].items():
            n[k] += v
    tot = defaultdict(int)
    for k in s:
        for c, v in s[k].items():
            tot[(k.split('|')[0], c)] += v
    print(lab, {f'{a}:{c}': round(v / 1e9, 2) for (a, c), v in sorted(tot.items())})
    for k in sorted(s, key=lambda k: -sum(s[k].values()))[:10]:
        print(f"   {k[:45]:45} n={n[k]:7} " + ' '.join(f"{c}={v/1e9:.2f}" for c, v in sorted(s[k].items())))

print('\n## 9. nowork sync duration histogram (counts), g1 -> g4')
for R, lab in ((A, 'g1'), (B, 'g4')):
    h = defaultdict(lambda: defaultdict(int))
    for q in qs:
        for k, d in R[q]['synch'].items():
            api, op, m = k.split('|')
            if m != 'nowork':
                continue
            for b, v in d.items():
                h[api][b] += v
    for api, d in h.items():
        print(lab, api, {b: d.get(b, 0) for b in ['lt5', 'lt20', 'lt50', 'lt200', 'lt1000', 'ge1000']})
