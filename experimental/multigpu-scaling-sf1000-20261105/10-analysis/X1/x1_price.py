#!/usr/bin/env python3
"""x1_price.py <map.csv> <out.json> [queries]: wall-share pricing of host-side excess at g4 (iter2 nsys windows).

Bins of BIN ns. Per bin: H2D GB/s (HtoD memcpy bytes spread over duration), mean #executor threads inside a Task NVTX
range, allocator excess (alloc/free duration minus a reference per-call cost), H1 excess (stream syncs that waited on a
cross-device *pooled* event = small-pinned slab events, minus a reference sync cost), allocator calls in flight.
Saving per bin = min(bin, excess_in_bin / active_threads); link-gated variants zero the saving in bins whose
H2D rate >= threshold (scan phases pinned to the 2-uplink ceiling)."""
import sqlite3, csv, sys, json, bisect, os
from collections import defaultdict

BIN = int(os.environ.get('BIN', 2_000_000))
REF = {'cudaMallocFromPoolAsync': (8000, 500), 'cudaFreeAsync': (2900, 300)}  # (g1 suite mean, sub-allocator)
TH = (70, 85, 95)


def analyze(db, w0, w1):
    c = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    S = dict(c.execute('select id,value from StringIds'))
    nb = (w1 - w0) // BIN + 1
    h2d = [0.0] * nb; act = [0.0] * nb; exg1 = [0.0] * nb; exsub = [0.0] * nb; h1 = [0.0] * nb; clone = [0.0] * nb
    acalls = [0] * nb

    def spread(arr, s, e, val):
        s = max(s, w0); e = min(e, w1)
        if e <= s:
            return
        tot = e - s
        b = (s - w0) // BIN
        while s < e:
            be = w0 + (b + 1) * BIN
            seg = min(e, be) - s
            arr[b] += val * seg / tot
            s += seg; b += 1

    for s, e, b in c.execute('select start,end,bytes from CUPTI_ACTIVITY_KIND_MEMCPY where copyKind in (1,10) and srcKind in (0,1) and dstKind=2 and end>=? and start<=?', (w0, w1)):
        spread(h2d, s, e, b)
    # task-active threads
    import re
    oprng = defaultdict(list); taskrng = defaultdict(list)
    for s, e, txt, tid, gt in c.execute('select start,end,text,textId,globalTid from NVTX_EVENTS where eventType=59 and end is not null and end>=? and start<=?', (w0, w1)):
        n = txt if txt is not None else S.get(tid, '')
        if n.startswith('Pipeline ') and ' Task ' in n:
            spread(act, s, e, (min(e, w1) - max(s, w0)) / BIN)
            taskrng[gt].append((s, e))
        else:
            m = re.match(r'Pipeline \d+: ([A-Z_]+) \(id=\d+\)', n)
            if m:
                oprng[gt].append((s, e, m.group(1)))
            elif n == 'sirius::compression::host_to_gpu':
                oprng[gt].append((s, e, 'prep:compressed_h2d'))
            elif n in ('convert:gpu_to_gpu', 'convert:host_to_gpu'):
                oprng[gt].append((s, e, 'prep:' + n.split(':')[1]))
    for d in (oprng, taskrng):
        for k in d:
            d[k].sort()
    opst = {k: [x[0] for x in v] for k, v in oprng.items()}
    tkst = {k: [x[0] for x in v] for k, v in taskrng.items()}
    bycls = defaultdict(lambda: [0.0] * nb)

    def cls_of(gt, s, e):
        v = oprng.get(gt)
        if v:
            i = bisect.bisect_right(opst[gt], s) - 1
            while i >= 0 and i >= len(v) - 10**9:
                if v[i][1] >= e:
                    return v[i][2]
                if v[i][0] < s - 5 * 10**9:
                    break
                i -= 1
                if i < 0 or opst[gt][i] < s - 10**9:
                    break
        t = taskrng.get(gt)
        if t:
            i = bisect.bisect_right(tkst[gt], s) - 1
            if i >= 0 and t[i][1] >= e:
                return 'task:other'
        return 'outside-task'
    ids = {i: v.split('_v')[0] for i, v in S.items()}
    for s, e, n, gt in c.execute("select start,end,nameId,globalTid from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (w0, w1)):
        nm = ids[n]
        if nm in REF:
            b = ((s + e) // 2 - w0) // BIN
            d = e - s
            x = (d - REF[nm][0]) if os.environ.get('SIGNED') else max(0, d - REF[nm][0])
            exg1[b] += x; exsub[b] += max(0, d - REF[nm][1]); acalls[b] += 1
            bycls[cls_of(gt, s, e)][b] += x
    # H1 + clone cross-device waits (same classification as x1_sync.py)
    ctxdev = dict(c.execute('select contextId,deviceId from TARGET_INFO_CUDA_CONTEXT_INFO'))
    sdev = {s_: ctxdev.get(cx) for s_, cx in c.execute('select streamId,contextId from TARGET_INFO_CUDA_STREAM')}
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
    sid = [i for i, v in ids.items() if v in ('cudaStreamSynchronize', 'cuStreamSynchronize')]
    last = {}
    for s, e, corr in c.execute(f"select start,end,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where nameId in ({','.join(map(str, sid))}) and start>=? and end<=? order by start", (w0, w1)):
        st = syncstream.get(corr)
        if st is None:
            continue
        en = ends.get(st, [])
        j = bisect.bisect_right(en, e + 5000) - 1
        work = j >= 0 and en[j] >= s
        prev = last.get(st, s - 10**7); last[st] = e
        w = waits.get(st, []); k = bisect.bisect_right(w, (s, float('inf'))) - 1
        kind = None
        while k >= 0 and w[k][0] >= prev:
            rs = w[k][1]
            if rs is not None and sdev.get(rs[0]) != sdev.get(st):
                kind = 'pooled' if nrec[rs[1]] >= 3 else 'oneshot'
                break
            k -= 1
        if kind is None:
            continue
        ref = 135000 if work else 15000
        ex = max(0, (e - s) - ref)
        spread(h1 if kind == 'pooled' else clone, s, e, ex)
    out = {'nb': nb, 'win': w1 - w0}
    for th in TH:
        lim = th * 1e9 * BIN / 1e9  # bytes per bin at th GB/s
        for nm, arr in (('alloc_g1', exg1), ('alloc_sub', exsub), ('h1', h1), ('clone', clone)):
            out[f'{nm}_gate{th}'] = sum(min(BIN, arr[i] / max(act[i], 1.0)) for i in range(nb) if h2d[i] < lim)
    for nm, arr in (('alloc_g1', exg1), ('alloc_sub', exsub), ('h1', h1), ('clone', clone)):
        out[f'{nm}_nogate'] = sum(min(BIN, arr[i] / max(act[i], 1.0)) for i in range(nb))
        out[f'{nm}_thread'] = sum(arr)
    # combined alloc_g1 + h1 (shared denominator, avoids double-count of the same bin time)
    for th in TH:
        lim = th * 1e9 * BIN / 1e9
        out[f'both_gate{th}'] = sum(min(BIN, (exg1[i] + h1[i]) / max(act[i], 1.0)) for i in range(nb) if h2d[i] < lim)
    R = float(os.environ.get('LINKCAP', 110)) * BIN  # bytes per bin at the measured ~105-110 GB/s two-uplink ceiling
    for nm, arr in (('alloc_g1', exg1), ('alloc_sub', exsub), ('h1', h1), ('clone', clone)):
        out[f'{nm}_soft'] = sum(min(arr[i] / max(act[i], 1.0), BIN * max(0.0, 1 - h2d[i] / R)) for i in range(nb))
    out['both_soft'] = sum(min((exg1[i] + h1[i]) / max(act[i], 1.0), BIN * max(0.0, 1 - h2d[i] / R)) for i in range(nb))
    out['bothsub_soft'] = sum(min((exsub[i] + h1[i]) / max(act[i], 1.0), BIN * max(0.0, 1 - h2d[i] / R)) for i in range(nb))
    out['h2d_bytes'] = sum(h2d)
    # per-class share of the alloc_g1 soft saving (bin saving split pro rata to class excess in that bin)
    for k, arr in bycls.items():
        tot = 0.0
        for i in range(nb):
            if exg1[i] > 0:
                sv = min(exg1[i] / max(act[i], 1.0), BIN * max(0.0, 1 - h2d[i] / R))
                tot += sv * arr[i] / exg1[i]
        out[f'cls_soft:{k}'] = tot
        out[f'cls_thread:{k}'] = sum(arr)
    out['act_mean'] = sum(act) / nb
    out['linkbound_frac85'] = sum(1 for i in range(nb) if h2d[i] >= 85 * BIN) / nb
    out['alloc_thread_in_link85'] = sum(exg1[i] for i in range(nb) if h2d[i] >= 85 * BIN)
    # allocator-saturated bins: calls/s >= 120k (profiled ceiling ~160-170k/s)
    out['alloc_sat_bins_frac'] = sum(1 for i in range(nb) if acalls[i] >= 120e3 * BIN / 1e9) / nb
    out['alloc_sat_bins_nonlink_frac'] = sum(1 for i in range(nb) if acalls[i] >= 120e3 * BIN / 1e9 and h2d[i] < 85 * BIN) / nb
    return out


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
