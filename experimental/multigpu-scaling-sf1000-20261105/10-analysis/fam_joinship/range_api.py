#!/usr/bin/env python3
"""range_api.py <sqlite> <start,end> <exact-range-name-prefix>: per matching NVTX range (same thread), API call counts/time,
kernel GPU time, mean per range. Prints totals and per-range means."""
import sqlite3, sys, bisect
from collections import Counter, defaultdict
c = sqlite3.connect(sys.argv[1]); w0, w1 = map(int, sys.argv[2].split(",")); pre = sys.argv[3]
S = dict(c.execute("select id,value from StringIds"))
rs = []
for s, e, t, tid, g in c.execute("select start,end,text,textId,globalTid from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (w0, w1)):
    n = t if t is not None else S.get(tid, "")
    if n.startswith(pre) and not n.endswith("sink"):
        rs.append((g, s, e))
tids = {r[0] for r in rs}
rt = defaultdict(list)
for s, e, g, nid, corr in c.execute("select start,end,globalTid,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (w0, w1)):
    if g in tids: rt[g].append((s, e, nid, corr))
for g in rt: rt[g].sort()
kern = dict(c.execute("select correlationId,end-start from CUPTI_ACTIVITY_KIND_KERNEL where start>=?", (w0,)))
cnt, tm = Counter(), Counter(); wall = 0; k = 0
for g, s, e in rs:
    wall += e - s
    L = rt[g]; i = bisect.bisect_left(L, (s,))
    while i < len(L) and L[i][0] < e:
        st, en, nid, corr = L[i]; nm = S.get(nid, str(nid)).split("_v")[0]
        cnt[nm] += 1; tm[nm] += en - st; k += kern.get(corr, 0); i += 1
n = len(rs)
print(f"ranges={n} wall={wall/1e6:.1f}ms mean={wall/1e6/max(n,1):.3f}ms kernels={k/1e6:.1f}ms")
for nm, t in tm.most_common(10):
    print(f"  {nm:30} calls={cnt[nm]:7d} ({cnt[nm]/max(n,1):6.1f}/range) {t/1e6:9.1f}ms mean={t/1e3/cnt[nm]:7.1f}us")
