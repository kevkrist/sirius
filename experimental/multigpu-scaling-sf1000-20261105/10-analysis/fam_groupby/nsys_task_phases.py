#!/usr/bin/env python3
"""nsys_task_phases.py <sqlite> <start> <end> <task-range substring> <op-range substring>
For each NVTX task range matching, split its wall on the same thread into: before op range (prep), inside op range, after
op range; and per segment sum the CUDA runtime API time by name (+counts), memcpy bytes launched. Prints the mean per task."""
import sqlite3, sys
from collections import Counter, defaultdict
db, W0, W1, tsub, osub = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
c = sqlite3.connect(db); S = dict(c.execute("select id,value from StringIds"))
R = []
for s, e, tid, t, ti in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (W0, W1)):
    R.append((s, e, tid, t if t is not None else S.get(ti, '')))
tasks = [r for r in R if tsub in r[3]]
ops = defaultdict(list); conv = defaultdict(list)
for r in R:
    if osub in r[3]: ops[r[2]].append(r)
    if r[3].startswith('convert:gpu_to_gpu') and r[3] == 'convert:gpu_to_gpu': conv[r[2]].append(r)
tids = {t[2] for t in tasks}
api = defaultdict(list)
for s, e, tid, n in c.execute("select start,end,globalTid,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (W0, W1)):
    if tid in tids: api[tid].append((s, e, S.get(n, n)))
osrt = defaultdict(list)
try:
    for s, e, tid, n in c.execute("select start,end,globalTid,nameId from OSRT_API where start>=? and end<=?", (W0, W1)):
        if tid in tids: osrt[tid].append((s, e, S.get(n, n)))
except Exception: pass
seg = defaultdict(float); segapi = defaultdict(Counter); segcnt = defaultdict(Counter); segos = defaultdict(Counter); nconv = 0; convms = 0
for (s, e, tid, name) in tasks:
    o = [x for x in ops[tid] if x[0] >= s and x[1] <= e]
    if not o: continue
    os_, oe = min(x[0] for x in o), max(x[1] for x in o)
    bounds = {'prep': (s, os_), 'op': (os_, oe), 'after': (oe, e)}
    cv = [x for x in conv[tid] if x[0] >= s and x[1] <= e]; nconv += len(cv); convms += sum(x[1] - x[0] for x in cv) / 1e6
    for k, (a, b) in bounds.items():
        seg[k] += (b - a) / 1e6
        for (x, y, n) in api[tid]:
            if x >= a and y <= b: segapi[k][n] += (y - x) / 1e6; segcnt[k][n] += 1
        for (x, y, n) in osrt[tid]:
            if x >= a and y <= b: segos[k][n] += (y - x) / 1e6
N = len(tasks)
print(f"{N} task ranges matching '{tsub}'; convert:gpu_to_gpu ranges per task {nconv/N:.0f}, ms/task {convms/N:.1f}")
for k in ('prep', 'op', 'after'):
    print(f" {k:5}: {seg[k]/N:7.1f} ms/task | api: " + ', '.join(f"{n.split('_v')[0]}={v/N:.1f}ms/{segcnt[k][n]/N:.0f}" for n, v in segapi[k].most_common(7)))
    print(f"        osrt: " + ', '.join(f"{n}={v/N:.1f}" for n, v in segos[k].most_common(4)))
