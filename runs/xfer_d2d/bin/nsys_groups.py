#!/usr/bin/env python3
"""nsys_groups.py <sqlite>: memcpy records issued by the finish thread inside accum::finish (hot iteration 1), grouped by API and attributes."""
import sqlite3, sys, collections
c = sqlite3.connect(sys.argv[1]).cursor()
S = dict(c.execute('select id,value from StringIds'))
nv = [(S.get(tid) if t is None else t, s, e, g) for t, tid, s, e, g in c.execute('select text,textId,start,end,globalTid from NVTX_EVENTS') if e is not None]
qw = sorted((s, e) for n, s, e, g in nv if n == 'sirius::query')
ws, we = qw[1]
fin = [(s, e, g) for n, s, e, g in nv if n == 'dynfilter::accum::finish' and ws <= s < we]
M = collections.defaultdict(list)
for row in c.execute('select correlationId,bytes,copyKind,srcKind,dstKind,srcDeviceId,dstDeviceId,copyCount,deviceId from CUPTI_ACTIVITY_KIND_MEMCPY'):
    M[row[0]].append(row[1:])
G = collections.defaultdict(lambda: [0, 0, 0]); A = collections.Counter()
for s, e, g in fin:
    for cs, ce, corr, nid in c.execute('select start,end,correlationId,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where globalTid=? and start>=? and start<?', (g, s, e)):
        name = S.get(nid, '?').split('_v')[0]; A[name] += 1
        for b, k, sk, dk, sd, dd, cc, dev in M.get(corr, []):
            x = G[(name, k, sk, dk, sd, dd, dev)]; x[0] += 1; x[1] += b; x[2] += cc or 0
print("API calls:", dict(A))
print("api | copyKind srcKind dstKind | srcDev -> dstDev (record deviceId) | records | MiB | copyCount")
for k, v in sorted(G.items(), key=lambda kv: -kv[1][1]):
    print(f"{k[0]} | {k[1]} {k[2]} {k[3]} | {k[4]} -> {k[5]} ({k[6]}) | {v[0]} | {v[1]/2**20:.0f} | {v[2]}")
