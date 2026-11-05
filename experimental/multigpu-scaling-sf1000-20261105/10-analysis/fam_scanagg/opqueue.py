#!/usr/bin/env python3
"""Where does the wall of an NVTX op range go?  For each range (same thread) we take the CUDA runtime calls inside it,
join them to GPU activities by correlationId, and report:
  - api_ms by function, sync_ms (cudaStreamSynchronize/cuStreamSynchronize)
  - per GPU activity: queue delay = gpu_start - api_end (time the op waited for the stream / engine), gpu duration
    split by activity type (kernel, memcpy kind incl. tiny copies)
usage: opqueue.py <sqlite> <start_ns> <end_ns> <nvtx substring>
"""
import sqlite3, sys, bisect, statistics as st
from collections import defaultdict, Counter
db, s0, e0, sub = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
c = sqlite3.connect(db)
S = dict(c.execute("select id,value from StringIds"))
rng = []
for s, e, tid, text, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (s0, e0)):
    n = text if text is not None else S.get(tx, "")
    if sub in n:
        rng.append((tid, s, e))
tids = {r[0] for r in rng}
calls = defaultdict(list)
for s, e, tid, nid, corr in c.execute("select start,end,globalTid,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (s0, e0)):
    if tid in tids:
        calls[tid].append((s, e, S.get(nid, str(nid)).split('_v')[0], corr))
for t in calls:
    calls[t].sort()
gpu = {}
for s, e, corr, dev, strm, nid in c.execute("select start,end,correlationId,deviceId,streamId,shortName from CUPTI_ACTIVITY_KIND_KERNEL where start>=?", (s0,)):
    gpu[corr] = ('kernel', s, e, 0, dev)
mk = {1: 'H2D', 2: 'D2H', 8: 'D2D', 10: 'P2P'}
for s, e, corr, dev, k, b, sk, dk in c.execute("select start,end,correlationId,deviceId,copyKind,bytes,srcKind,dstKind from CUPTI_ACTIVITY_KIND_MEMCPY where start>=?", (s0,)):
    kind = mk.get(k, str(k))
    if sk == 1 and dk == 2: kind = 'host->dev'
    if sk == 2 and dk == 1: kind = 'dev->host'
    if sk == 0 or dk == 0: kind += '(pageable)'
    gpu[corr] = ('memcpy:' + kind + (':tiny' if b < 65536 else ':big'), s, e, b, dev)
for s, e, corr, dev in c.execute("select start,end,correlationId,deviceId from CUPTI_ACTIVITY_KIND_MEMSET where start>=?", (s0,)) if 'CUPTI_ACTIVITY_KIND_MEMSET' in {r[0] for r in c.execute("select name from sqlite_master")} else []:
    gpu[corr] = ('memset', s, e, 0, dev)
api = Counter(); apin = Counter(); qd = defaultdict(list); gd = defaultdict(float); gn = Counter()
wall = 0
for tid, s, e in rng:
    wall += e - s
    cl = calls[tid]; ks = [x[0] for x in cl]
    for cs, ce, n, corr in cl[bisect.bisect_left(ks, s):bisect.bisect_right(ks, e)]:
        if ce > e:
            continue
        api[n] += ce - cs; apin[n] += 1
        g = gpu.get(corr)
        if g:
            qd[g[0]].append((g[1] - ce) / 1e6); gd[g[0]] += (g[2] - g[1]) / 1e6; gn[g[0]] += 1
print(f"ranges '{sub}': n={len(rng)} wall {wall / 1e6:.1f} ms ({wall / 1e6 / max(len(rng), 1):.3f} ms/range)")
print(" api ms (count): " + ", ".join(f"{k}={v / 1e6:.1f}({apin[k]})" for k, v in api.most_common(10)))
for k in sorted(qd, key=lambda k: -sum(qd[k])):
    v = qd[k]
    print(f"  {k:32s} n={gn[k]:6d} gpu_dur {gd[k]:8.1f} ms  queue_delay med {st.median(v):.3f} p90 {sorted(v)[int(.9 * len(v))]:.3f} sum {sum(v):.1f} ms")
