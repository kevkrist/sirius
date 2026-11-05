#!/usr/bin/env python3
"""overlap_dir.py <map.csv> <iter>: (1) per switch, overlap of clone-H2D union with non-clone H2D (scan) union;
(2) staged-clone issued-not-started latency split by leg direction (d2h = foreign-stream leg, h2d)."""
import sqlite3, sys, csv, bisect
from collections import defaultdict
mp, IT = sys.argv[1], sys.argv[2]
SW = lambda d: 0 if d in (0, 1) else 1
def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out
def inter(A, B):
    i = j = 0; t = 0
    while i < len(A) and j < len(B):
        t += max(0, min(A[i][1], B[j][1]) - max(A[i][0], B[j][0]))
        if A[i][1] < B[j][1]: i += 1
        else: j += 1
    return t
TOT = defaultdict(float)
for r in [x for x in csv.DictReader(open(mp)) if x["iter"] == IT]:
    q = int(r["query"]); W0, W1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    c = sqlite3.connect(r["sqlite"]); S = dict(c.execute("select id,value from StringIds"))
    cl = defaultdict(list)
    for s, e, tid, t, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (W0, W1)):
        n = t if t is not None else S.get(tx, "")
        if n == "convert:gpu_to_gpu:host_staging": cl[tid].append((s, e))
    for v in cl.values(): v.sort()
    cids = {}
    for s, e, tid, cid in c.execute("select start,end,globalTid,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (W0, W1)):
        L = cl.get(tid)
        if not L: continue
        i = bisect.bisect_right(L, (s, 1 << 62)) - 1
        if i >= 0 and L[i][0] <= s <= L[i][1]: cids[cid] = s
    clone_h2d = defaultdict(list); other_h2d = defaultdict(list); gaps = defaultdict(float); prev_end = {}
    rows = list(c.execute("select start,end,deviceId,srcKind,dstKind,correlationId,streamId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and start<=? order by start", (W0, W1)))
    for s, e, dev, sk, dk, cid, sid in rows:
        if dk == 2 and sk != 2:
            (clone_h2d if cid in cids else other_h2d)[SW(dev)].append((s, e))
        if cid in cids:
            d = "d2h" if sk == 2 else "h2d"
            key = (sid, dev, d)
            st = max(cids[cid], prev_end.get(sid, 0))
            if s > st: gaps[d] += s - st; gaps[d + "_n"] += 1
            prev_end[sid] = e
    for sw in (0, 1):
        A = union(clone_h2d[sw]); B = union(other_h2d[sw])
        TOT["ov"] += inter(A, B) / 1e6; TOT["clone_h2d_union"] += sum(y - x for x, y in A) / 1e6
    for k, v in gaps.items(): TOT["gap_" + k] += v / 1e6 if not k.endswith("_n") else v
print({k: round(v, 1) for k, v in TOT.items()})
print("per-copy start latency after max(issue, prev own copy end) [us]: d2h %.1f  h2d %.1f" % (
    TOT["gap_d2h"] * 1e3 / max(TOT["gap_d2h_n"], 1), TOT["gap_h2d"] * 1e3 / max(TOT["gap_h2d_n"], 1)))
