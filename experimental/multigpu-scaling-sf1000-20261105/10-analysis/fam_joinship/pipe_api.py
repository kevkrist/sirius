#!/usr/bin/env python3
"""pipe_api.py <sqlite> <start,end>: per pipeline (from 'Pipeline K Task N [..]' NVTX ranges), task thread-ms and the
thread-ms spent inside: cudaMallocFromPoolAsync+cudaFreeAsync (alloc), *StreamSynchronize (sync), launched kernel GPU ms."""
import sqlite3, sys, bisect, re
from collections import defaultdict
c = sqlite3.connect(sys.argv[1]); w0, w1 = map(int, sys.argv[2].split(","))
S = dict(c.execute("select id,value from StringIds"))
tasks = defaultdict(list); lab = {}
for s, e, t, tid, g in c.execute("select start,end,text,textId,globalTid from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (w0, w1)):
    n = t if t is not None else S.get(tid, "")
    m = re.match(r"Pipeline (\d+) Task \d+ \[(.*)\]", n)
    if m: tasks[g].append((s, e, int(m.group(1)))); lab[int(m.group(1))] = m.group(2).split(" -> ")[0]
rt = defaultdict(list)
for s, e, g, nid, corr in c.execute("select start,end,globalTid,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (w0, w1)):
    if g in tasks: rt[g].append((s, e, S.get(nid, ""), corr))
kern = dict(c.execute("select correlationId,end-start from CUPTI_ACTIVITY_KIND_KERNEL where start>=?", (w0,)))
P = defaultdict(lambda: defaultdict(float))
for g, T in tasks.items():
    L = sorted(rt[g]); st = [x[0] for x in L]
    for s, e, p in T:
        a = P[p]; a["n"] += 1; a["task"] += (e - s) / 1e6
        for x in L[bisect.bisect_left(st, s):bisect.bisect_left(st, e)]:
            d = (x[1] - x[0]) / 1e6
            if "MallocFromPool" in x[2] or "FreeAsync" in x[2]: a["alloc"] += d; a["nalloc"] += 1
            elif "StreamSynchronize" in x[2]: a["sync"] += d
            a["kern"] += kern.get(x[3], 0) / 1e6
print(f"{'pipe':>4} {'op':14} {'tasks':>5} {'task_ms':>8} {'alloc_ms':>8} {'n_alloc':>7} {'alloc%':>6} {'sync_ms':>8} {'kern_ms':>8}")
for p in sorted(P):
    a = P[p]
    if a["task"] < 5: continue
    print(f"{p:4d} {lab[p][:14]:14} {a['n']:5.0f} {a['task']:8.1f} {a['alloc']:8.1f} {a['nalloc']:7.0f} {100*a['alloc']/a['task']:6.1f} {a['sync']:8.1f} {a['kern']:8.1f}")
