#!/usr/bin/env python3
"""syncwait.py <sqlite> <nvtx substr> : for long stream syncs (>=1ms) inside matching NVTX ranges (WINDOW env),
show the synced stream/device and what GPU work on that stream ran during the wait (kernels/memcpy by kind, bytes)."""
import sqlite3, sys, os
from collections import Counter
db, sub = sys.argv[1], sys.argv[2]
W = tuple(int(x) for x in os.environ["WINDOW"].split(","))
c = sqlite3.connect(db); S = dict(c.execute("select id,value from StringIds"))
rng = []
for st, en, tid, text, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where end is not null and eventType=59 and start>=? and end<=?", W):
    n = text if text is not None else S.get(tx, "")
    if sub in n: rng.append((tid, st, en, n))
syn = list(c.execute("select start,end,streamId,deviceId,correlationId from CUPTI_ACTIVITY_KIND_SYNCHRONIZATION where start>=? and end<=? and end-start>=1000000", W))
rt = {r[0]: r[1] for r in c.execute("select correlationId, globalTid from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", W)}
tot = 0; kinds = Counter()
for s, e, strm, dev, cor in syn:
    tid = rt.get(cor)
    if not any(t == tid and a <= s and e <= b for t, a, b, _ in rng): continue
    tot += (e - s)
    mc = list(c.execute("select copyKind,bytes,start,end from CUPTI_ACTIVITY_KIND_MEMCPY where streamId=? and deviceId=? and end>? and start<?", (strm, dev, s, e)))
    kn = list(c.execute("select start,end from CUPTI_ACTIVITY_KIND_KERNEL where streamId=? and deviceId=? and end>? and start<?", (strm, dev, s, e)))
    km = sum(min(b, e) - max(a, s) for a, b in kn) / 1e6
    by = Counter(); bt = Counter()
    for k, b, a, z in mc: by[k] += b; bt[k] += (min(z, e) - max(a, s)) / 1e6
    print(f"sync {(e-s)/1e6:6.2f} ms dev{dev} stream{strm}: kernels {km:5.2f} ms; memcpy " + ", ".join(f"kind{k} {by[k]/1e9:.3f}GB {bt[k]:.2f}ms" for k in by))
    for k in by: kinds[k] += bt[k]
print(f"total long-sync {tot/1e6:.1f} ms; memcpy-on-same-stream ms by kind {dict(kinds)} (1=HtoD 2=DtoH 8=DtoD 10=PtoP)")
