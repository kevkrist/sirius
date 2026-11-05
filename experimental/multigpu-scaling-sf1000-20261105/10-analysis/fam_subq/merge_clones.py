#!/usr/bin/env python3
"""Per MERGE_GROUP_BY task (nsys Task range): wall and the convert:gpu_to_gpu clone ranges on the same thread.
usage: merge_clones.py <map.csv> <query>"""
import csv, sqlite3, sys
from collections import defaultdict

mp, q = sys.argv[1], sys.argv[2]
row = [r for r in csv.DictReader(open(mp)) if r['query'] == q and r['iter'] == '2'][0]
w0, w1 = int(row['range_start_ns']), int(row['range_end_ns'])
c = sqlite3.connect(row['sqlite'])
S = dict(c.execute("select id,value from StringIds"))
merge, conv = [], defaultdict(list)
for s, e, tid, t, ti in c.execute(
        "select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?",
        (w0, w1)):
    nm = t if t is not None else S.get(ti, '')
    if 'MERGE_GROUP_BY' in nm and 'Task' in nm:
        merge.append((s, e, tid, nm))
    if nm.startswith('convert:gpu_to_gpu'):
        conv[nm].append((s, e, tid))
for s, e, tid, nm in sorted(merge):
    out = {}
    for k, v in conv.items():
        inside = [x for x in v if x[2] == tid and x[0] >= s and x[1] <= e]
        out[k.split(':')[-1]] = (len(inside), round(sum(x[1] - x[0] for x in inside) / 1e6, 1))
    print(f"{nm[:60]} start={(s - w0) / 1e6:.0f}ms wall={(e - s) / 1e6:.1f}ms clones(n,ms)={out}")
