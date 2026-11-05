#!/usr/bin/env python3
"""x2_syncwait.py <map.csv> <queries comma> [iter=2]: inside each convert:gpu_to_gpu:{host_staging,peer_copy} range,
split every cuStreamSynchronize/cudaStreamSynchronize into (a) waiting for the clone's own DMA (until max end of own
memcpys issued before the sync) and (b) excess = sync_end - max(sync_start, own_dma_end) (driver latency + foreign work
queued on the shared pooled stream). Also: kernels from OTHER threads on the same (device,stream) overlapping the clone."""
import sqlite3, sys, csv, bisect
from collections import defaultdict, Counter
mp, QS = sys.argv[1], set(sys.argv[2].split(",")); IT = sys.argv[3] if len(sys.argv) > 3 else "2"
for r in csv.DictReader(open(mp)):
    if r["iter"] != IT or r["query"] not in QS: continue
    W0, W1 = int(r["range_start_ns"]), int(r["range_end_ns"]); c = sqlite3.connect(r["sqlite"])
    S = dict(c.execute("select id,value from StringIds"))
    conv = [(s, e, tid, t if t is not None else S.get(tx, "")) for s, e, tid, t, tx in c.execute(
        "select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (W0, W1))]
    conv = [x for x in conv if x[3] in ("convert:gpu_to_gpu:host_staging", "convert:gpu_to_gpu:peer_copy")]
    tids = {x[2] for x in conv}
    rt = defaultdict(list)
    for s, e, tid, nid, corr in c.execute("select start,end,globalTid,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (W0, W1)):
        if tid in tids: rt[tid].append((s, e, S.get(nid, ""), corr))
    for v in rt.values(): v.sort()
    mem = {corr: (s, e, dev, st) for s, e, corr, dev, st in c.execute("select start,end,correlationId,deviceId,streamId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?", (W0, W1))}
    K = defaultdict(list)
    for s, e, dev, st, corr in c.execute("select start,end,deviceId,streamId,correlationId from CUPTI_ACTIVITY_KIND_KERNEL where start>=? and end<=?", (W0, W1)):
        K[(dev, st)].append((s, e, corr))
    for v in K.values(): v.sort()
    own_corr_all = set()
    for v in rt.values(): own_corr_all.update(x[3] for x in v)
    A = defaultdict(Counter)
    for s, e, tid, nm in conv:
        k = "staged" if nm.endswith("staging") else "peer"; a = A[k]; a["n"] += 1; a["ms"] += (e - s) / 1e6
        calls = rt[tid][bisect.bisect_left(rt[tid], (s,)):bisect.bisect_right(rt[tid], (e, 1 << 62))]
        own_end = 0; streams = set()
        for cs, ce, cn, corr in calls:
            m = mem.get(corr)
            if m: own_end = max(own_end, m[1]); streams.add((m[2], m[3]))
            if "StreamSynchronize" in cn:
                a["sync"] += 1; a["sync_ms"] += (ce - cs) / 1e6
                a["own_wait_ms"] += max(0, min(ce, own_end) - cs) / 1e6
                a["excess_ms"] += (ce - max(cs, own_end)) / 1e6 if ce > max(cs, own_end) else 0
        for ds in streams:
            for ks, ke, kc in K.get(ds, [])[max(0, bisect.bisect_left(K.get(ds, []), (s,)) - 50):]:
                if ks > e: break
                if ke > s and kc not in own_corr_all:
                    a["foreign_kern_ms"] += (min(ke, e) - max(ks, s)) / 1e6; a["foreign_kern_n"] += 1
        a["clones_with_foreign"] += 0
    for k, a in A.items():
        print(f"Q{r['query']} {k}: n={a['n']} clone_ms={a['ms']:.0f} syncs={a['sync']} sync_ms={a['sync_ms']:.0f} own_dma_wait_ms={a['own_wait_ms']:.0f} "
              f"excess_ms={a['excess_ms']:.0f} ({a['excess_ms']/max(1,a['sync'])*1e3:.0f} us/sync) foreign_kernels_on_clone_stream n={a['foreign_kern_n']} ms={a['foreign_kern_ms']:.1f}", flush=True)
