#!/usr/bin/env python3
"""For each mapped iter-2 query report: activity in the report BEFORE the sirius::query range (capture start ->
range_start) and AFTER it (range_end -> next/last event). usage: pre_window.py <map.csv> [queries]"""
import csv, sqlite3, sys
from collections import Counter
m = [r for r in csv.DictReader(open(sys.argv[1])) if r["iter"] == "2"]
qs = set(sys.argv[2].split(",")) if len(sys.argv) > 2 else None
for r in m:
    if qs and r["query"] not in qs: continue
    c = sqlite3.connect(r["sqlite"]); S = dict(c.execute("select id,value from StringIds"))
    s, e = int(r["range_start_ns"]), int(r["range_end_ns"])
    # other sirius::query ranges in this report bound the "pre" window of this query
    qid = [k for k, v in S.items() if v == "sirius::query"]
    rngs = sorted(c.execute(f"select start,end from NVTX_EVENTS where textId in ({','.join(map(str,qid))}) and end is not null").fetchall())
    prev_end = max([b for a, b in rngs if b <= s], default=0)
    pre = (prev_end, s)
    def cnt(tbl, lo, hi):
        try: return c.execute(f"select count(*), coalesce(sum(end-start),0) from {tbl} where start>=? and start<?", (lo, hi)).fetchone()
        except Exception: return (0, 0)
    rt = cnt("CUPTI_ACTIVITY_KIND_RUNTIME", *pre); k = cnt("CUPTI_ACTIVITY_KIND_KERNEL", *pre); mc = cnt("CUPTI_ACTIVITY_KIND_MEMCPY", *pre)
    nv = Counter()
    for t, tid, a, b in c.execute("select text,textId,start,end from NVTX_EVENTS where start>=? and start<? and end is not null", pre):
        nv[(t or S.get(tid, "?"))[:40]] += (b - a) / 1e6
    top_rt = c.execute("select nameId,count(*),sum(end-start) from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and start<? group by nameId order by 3 desc limit 3", pre).fetchall()
    print(f"Q{r['query']}: pre-window {(pre[1]-pre[0])/1e6:.1f} ms; cuda rt {rt[0]} calls {rt[1]/1e6:.2f} ms; kernels {k[0]} ({k[1]/1e6:.2f} ms); memcpy {mc[0]} ({mc[1]/1e6:.2f} ms); "
          f"top rt {[(S.get(a,'?')[:28], n, round(t/1e6,2)) for a,n,t in top_rt]}; nvtx {dict((k_, round(v,1)) for k_, v in nv.most_common(4))}")
