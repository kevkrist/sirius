#!/usr/bin/env python3
"""x1_sync.py <map.csv> <out.json> [queries]: classify every stream sync (cuda/cuStreamSynchronize) in the iter2 window by
(a) whether GPU work on the synced stream ended inside the call ('work') or not ('nowork'), and
(b) the most recent cudaStreamWaitEvent on that stream since its previous sync: none / same-device / cross-device
    (recording stream's device != synced stream's device; CUPTI CUDA_EVENT links eventId+eventSyncId to its stream).
Sums thread-ns per (api, cls, relation) and per duration bucket."""
import sqlite3, csv, sys, json, bisect
from collections import defaultdict

SY = ('cudaStreamSynchronize', 'cuStreamSynchronize')


def analyze(db, w0, w1):
    c = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    S = dict(c.execute('select id,value from StringIds'))
    ctxdev = dict(c.execute('select contextId,deviceId from TARGET_INFO_CUDA_CONTEXT_INFO'))
    sdev = {s: ctxdev.get(cx) for s, cx in c.execute('select streamId,contextId from TARGET_INFO_CUDA_STREAM')}
    ends = defaultdict(list)
    for tbl in ('CUPTI_ACTIVITY_KIND_KERNEL', 'CUPTI_ACTIVITY_KIND_MEMCPY', 'CUPTI_ACTIVITY_KIND_MEMSET'):
        for e, st, d in c.execute(f'select end,streamId,deviceId from {tbl} where end>=? and start<=?', (w0 - 10**9, w1)):
            ends[st].append(e); sdev.setdefault(st, d)
    for st in ends:
        ends[st].sort()
    evs = {}; nrec = defaultdict(int)
    for st, e, s_ in c.execute('select streamId,eventId,eventSyncId from CUPTI_ACTIVITY_KIND_CUDA_EVENT'):
        evs[(e, s_)] = (st, e); nrec[e] += 1
    waits = defaultdict(list)
    for t, st, e, s_ in c.execute('select start,streamId,eventId,eventSyncId from CUPTI_ACTIVITY_KIND_SYNCHRONIZATION where syncType=2 and start>=? and start<=?', (w0 - 10**8, w1)):
        waits[st].append((t, evs.get((e, s_))))
    for st in waits:
        waits[st].sort()
    syncstream = dict(c.execute('select correlationId,streamId from CUPTI_ACTIVITY_KIND_SYNCHRONIZATION where syncType=3 and start>=? and end<=?', (w0, w1)))
    ids = {i: v.split('_v')[0] for i, v in S.items() if v.split('_v')[0] in SY}
    rows = c.execute(f"select start,end,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where nameId in ({','.join(map(str, ids))}) and start>=? and end<=? order by start", (w0, w1)).fetchall()
    last_sync = {}
    out = defaultdict(lambda: [0, 0])
    for s, e, n, corr in rows:
        st = syncstream.get(corr)
        if st is None:
            continue
        en = ends.get(st, [])
        j = bisect.bisect_right(en, e + 5000) - 1
        cls = 'work' if (j >= 0 and en[j] >= s) else 'nowork'
        prev = last_sync.get(st, s - 10**7)
        rel = 'none'
        w = waits.get(st, [])
        k = bisect.bisect_right(w, (s, float('inf'))) - 1
        while k >= 0 and w[k][0] >= prev:
            rs = w[k][1]
            if rs is not None:
                rs, eid = rs
            r = 'unknown' if rs is None else ('same-stream' if rs == st else ('same-dev' if sdev.get(rs) == sdev.get(st) else ('cross-dev-pooled' if nrec[eid] >= 3 else 'cross-dev-oneshot')))
            if r.startswith('cross-dev') or rel == 'none' or (r == 'same-dev' and not rel.startswith('cross-dev')):
                rel = r if not rel.startswith('cross-dev') else rel
            k -= 1
        last_sync[st] = e
        d = e - s
        b = 'lt20' if d < 20000 else 'lt200' if d < 200000 else 'lt1000' if d < 1000000 else 'ge1000'
        for key in (f'{ids[n]}|{cls}|{rel}', f'{ids[n]}|{cls}|{rel}|{b}'):
            out[key][0] += 1; out[key][1] += d
    return dict(out)


if __name__ == '__main__':
    mp, outp = sys.argv[1], sys.argv[2]
    qs = {int(x) for x in sys.argv[3:]}
    res = {}
    for r in csv.DictReader(open(mp)):
        if r['iter'] != '2' or (qs and int(r['query']) not in qs):
            continue
        res[int(r['query'])] = analyze(r['sqlite'], int(r['range_start_ns']), int(r['range_end_ns']))
        print('done', r['query'], file=sys.stderr, flush=True)
    json.dump(res, open(outp, 'w'))
