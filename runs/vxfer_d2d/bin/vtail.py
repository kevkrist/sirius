#!/usr/bin/env python3
"""vtail.py <nsys.sqlite>: per hot query window, for every dynfilter::accum::finish range: (a) latest end of any copy or kernel issued by
the finish thread inside the range, relative to the range end (must be <= 0: nothing may be in flight when finish returns);
(b) wall between the end of the last finish and the end of the query; (c) overlap of the finish ranges with each other."""
import sqlite3, sys
c = sqlite3.connect(sys.argv[1]).cursor()
S = dict(c.execute('select id,value from StringIds'))
nv = [(S.get(tid) if t is None else t, s, e, g) for t, tid, s, e, g in c.execute('select text,textId,start,end,globalTid from NVTX_EVENTS') if e is not None]
qw = sorted((s, e) for n, s, e, g in nv if n == 'sirius::query')
fin = sorted((s, e, g) for n, s, e, g in nv if n == 'dynfilter::accum::finish')
rt = list(c.execute('select start,end,globalTid,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME'))
mem = {}
for s, e, corr in c.execute('select start,end,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY'): mem.setdefault(corr, []).append(e)
for s, e, corr in c.execute('select start,end,correlationId from CUPTI_ACTIVITY_KIND_KERNEL'): mem.setdefault(corr, []).append(e)
for wi, (ws, we) in enumerate(qw):
    if wi == 0: continue
    fs = [f for f in fin if ws <= f[0] < we]
    out = []
    for s, e, g in fs:
        last = max([x for rs, re_, rg, corr in rt if rg == g and s <= rs <= e for x in mem.get(corr, [])], default=s)
        out.append(f"range {(s-ws)/1e6:.1f}-{(e-ws)/1e6:.1f} ms: last GPU op ends {(last-e)/1e6:+.3f} ms vs range end")
    ov = sum(max(0, min(a[1], b[1]) - max(a[0], b[0])) for i, a in enumerate(fs) for b in fs[i+1:]) / 1e6
    print(f"iter {wi}: wall {(we-ws)/1e6:.1f}; " + "; ".join(out) + f"; finish-finish overlap {ov:.1f} ms; last finish end -> query end {(we-max(f[1] for f in fs))/1e6:.1f} ms")
