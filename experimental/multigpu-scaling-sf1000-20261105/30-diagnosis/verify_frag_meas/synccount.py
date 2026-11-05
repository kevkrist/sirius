"""count runtime API calls (by name) inside NVTX ranges matching exact names, within WINDOW. usage: synccount.py db start end name..."""
import sqlite3, sys, bisect, collections
db, s0, e0 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); names = sys.argv[4:]
c = sqlite3.connect(db); S = dict(c.execute("select id,value from StringIds"))
R = collections.defaultdict(list)
for st, en, tid, text, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (s0, e0)):
    n = text if text is not None else S.get(tx, '')
    if n in names: R[tid].append((st, en, n))
cnt = collections.defaultdict(collections.Counter); nr = collections.Counter(); wall = collections.Counter()
for tid, L in R.items():
    for st, en, n in L: nr[n] += 1; wall[n] += (en - st) / 1e6
    L.sort(); starts = [x[0] for x in L]
    for st, en, nid in c.execute("select start,end,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where globalTid=? and start>=? and end<=?", (tid, s0, e0)):
        i = bisect.bisect_right(starts, st) - 1
        # check all ranges that could contain (nested ranges of different names): scan back a few
        j = i
        while j >= 0 and j > i - 4:
            a, b, n = L[j]
            if a <= st and en <= b: cnt[n][S.get(nid, nid)] += 1
            j -= 1
for n in names:
    c_ = cnt[n]; k = nr[n] or 1
    print(f'{n}: n={nr[n]} wall={wall[n]:.1f}ms per-call: ' + ', '.join(f'{a.split("_v")[0]}={b/k:.2f}' for a, b in c_.most_common(8)))
