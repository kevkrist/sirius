#!/usr/bin/env python3
"""x1_collateral.py <map.csv> [queries]: latency of non-allocator CUDA calls vs number of allocator calls (malloc/free)
in flight at their start (other threads). Shows whether the driver lock taken by the allocator also delays syncs,
memcpy issue, launches. Stream syncs restricted to 'nowork' (no GPU op on the synced stream ended inside the call)."""
import sqlite3, csv, sys, bisect
from collections import defaultdict
A = ('cudaMallocFromPoolAsync', 'cudaFreeAsync')
T = ('cudaStreamSynchronize', 'cuStreamSynchronize', 'cudaMemcpyAsync', 'cudaMemcpyBatchAsync', 'cudaLaunchKernel', 'cudaEventRecord', 'cudaStreamWaitEvent', 'cudaEventCreateWithFlags')
def bk(k): return '0' if k == 0 else '1-2' if k <= 2 else '3-5' if k <= 5 else '6-9' if k <= 9 else '10+'
qs = {int(x) for x in sys.argv[2:]}
agg = defaultdict(lambda: [0, 0])
for r in csv.DictReader(open(sys.argv[1])):
    if r['iter'] != '2' or (qs and int(r['query']) not in qs): continue
    w0, w1 = int(r['range_start_ns']), int(r['range_end_ns'])
    c = sqlite3.connect(f"file:{r['sqlite']}?mode=ro", uri=True)
    S = {i: v.split('_v')[0] for i, v in c.execute('select id,value from StringIds')}
    rows = [(s, e, S[n], corr) for s, e, n, corr in c.execute('select start,end,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?', (w0, w1))]
    al = [(s, e) for s, e, n, _ in rows if n in A]
    st = sorted(x[0] for x in al); en = sorted(x[1] for x in al)
    sstream = dict(c.execute('select correlationId,streamId from CUPTI_ACTIVITY_KIND_SYNCHRONIZATION where syncType=3 and start>=? and end<=?', (w0, w1)))
    ends = defaultdict(list)
    for tbl in ('CUPTI_ACTIVITY_KIND_KERNEL', 'CUPTI_ACTIVITY_KIND_MEMCPY', 'CUPTI_ACTIVITY_KIND_MEMSET'):
        for e, sid in c.execute(f'select end,streamId from {tbl} where end>=? and start<=?', (w0 - 10**9, w1)):
            ends[sid].append(e)
    for k in ends: ends[k].sort()
    for s, e, n, corr in rows:
        if n not in T: continue
        if 'Synchronize' in n:
            sid = sstream.get(corr)
            if sid is None: continue
            L = ends.get(sid, []); j = bisect.bisect_right(L, e + 5000) - 1
            if j >= 0 and L[j] >= s: continue
        k = bisect.bisect_right(st, s) - bisect.bisect_right(en, s)
        a = agg[(n, bk(k))]; a[0] += 1; a[1] += e - s
for n in T:
    print(f'{n[:26]:26}', ' '.join(f"{b}:{agg[(n,b)][1]/max(agg[(n,b)][0],1)/1e3:6.1f}us(n={agg[(n,b)][0]})" for b in ['0', '1-2', '3-5', '6-9', '10+']))
