#!/usr/bin/env python3
"""nsys_range_breakdown.py <report.sqlite> <nvtx-name-substring> [more substrings...]

For every NVTX push/pop range whose name contains a substring: wall time, plus what happened on the SAME
thread inside it: CUDA runtime API time by function (cudaStreamSynchronize, cudaStreamWaitEvent, ...),
GPU time of the kernels / memcpys / memsets those calls launched (via correlationId), and OSRT time
(pthread_mutex_lock, etc). Prints aggregate per range-name: count, wall ms, launched-kernel ms,
launched-memcpy ms (by kind), top runtime APIs, top OSRT calls.

Env WINDOW=start_ns,end_ns restricts to ranges inside that report-relative window (use map.csv).
Interpretation: kernel ms << wall ms with large sync/OSRT time => host-side / waiting; kernel ms ~ wall => GPU-bound.
"""
import sqlite3, sys
from collections import Counter, defaultdict

import os
db, subs = sys.argv[1], sys.argv[2:]
WIN = tuple(int(x) for x in os.environ["WINDOW"].split(",")) if os.environ.get("WINDOW") else None  # report-relative ns window (map.csv range_start_ns,range_end_ns)
c = sqlite3.connect(db)
S = dict(c.execute("select id, value from StringIds"))
tables = {r[0] for r in c.execute("select name from sqlite_master where type='table'")}
ranges = []
for start, end, tid, text, textId in c.execute(
        "select start, end, globalTid, text, textId from NVTX_EVENTS where end is not null and eventType=59"):
    name = text if text is not None else S.get(textId, "")
    if WIN and not (start >= WIN[0] and end <= WIN[1]):
        continue
    for s in subs:
        if s in name:
            ranges.append((tid, start, end, name)); break
by_tid = defaultdict(list)
for r in ranges:
    by_tid[r[0]].append(r)

rt = defaultdict(list)  # tid -> (start,end,nameId,corr)
for start, end, tid, nameId, corr in c.execute("select start,end,globalTid,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME"):
    if tid in by_tid:
        rt[tid].append((start, end, nameId, corr))
gpu_k = {corr: (end - start) for start, end, corr in c.execute("select start,end,correlationId from CUPTI_ACTIVITY_KIND_KERNEL")}
gpu_m = {corr: (end - start, kind, b) for start, end, corr, kind, b in
         c.execute("select start,end,correlationId,copyKind,bytes from CUPTI_ACTIVITY_KIND_MEMCPY")}
mk = dict(c.execute("select id,label from ENUM_CUDA_MEMCPY_OPER")) if "ENUM_CUDA_MEMCPY_OPER" in tables else {}
osrt = defaultdict(list)
if "OSRT_API" in tables:
    for start, end, tid, nameId in c.execute("select start,end,globalTid,nameId from OSRT_API"):
        if tid in by_tid:
            osrt[tid].append((start, end, nameId))

agg = defaultdict(lambda: dict(n=0, wall=0, kern=0, memcpy=Counter(), mbytes=Counter(), api=Counter(), os=Counter()))
import bisect
for tid, rs in by_tid.items():
    calls = sorted(rt[tid]); starts = [x[0] for x in calls]
    oc = sorted(osrt[tid]); ostarts = [x[0] for x in oc]
    for _, s, e, name in rs:
        key = name.split(" (id=")[0] if name.startswith("Pipeline") else name
        a = agg[key]; a["n"] += 1; a["wall"] += e - s
        for st, en, nid, corr in calls[bisect.bisect_left(starts, s):bisect.bisect_right(starts, e)]:
            a["api"][S.get(nid, nid)] += en - st
            if corr in gpu_k: a["kern"] += gpu_k[corr]
            if corr in gpu_m:
                d, kind, b = gpu_m[corr]; a["memcpy"][mk.get(kind, kind)] += d; a["mbytes"][mk.get(kind, kind)] += b
        for st, en, nid in oc[bisect.bisect_left(ostarts, s):bisect.bisect_right(ostarts, e)]:
            a["os"][S.get(nid, nid)] += en - st
for k, a in sorted(agg.items(), key=lambda x: -x[1]["wall"]):
    print(f"== {k}: n={a['n']} wall={a['wall']/1e6:.1f}ms launched_kernels={a['kern']/1e6:.1f}ms")
    for kind, d in a["memcpy"].most_common():
        print(f"   memcpy {kind}: {d/1e6:.1f}ms {a['mbytes'][kind]/1e9:.2f}GB ({a['mbytes'][kind]/max(d,1):.1f} GB/s)")
    print("   api: " + ", ".join(f"{n.split('_v')[0]}={d/1e6:.1f}" for n, d in a["api"].most_common(7)))
    if a["os"]:
        print("   osrt: " + ", ".join(f"{n}={d/1e6:.1f}" for n, d in a["os"].most_common(5)))
