#!/usr/bin/env python3
"""link_per_query.py <map.csv> <out.csv> [iter=2]

Per query (map row with the given iter), restricted to [range_start_ns, range_end_ns]:
  - host->device bytes (srcKind in {Pageable,Pinned}, dstKind=Device), regardless of copyKind
    (pinned host blocks registered under another GPU's context show as copyKind PtoP=10).
  - split by launching NVTX context: scan (sirius::compression::host_to_gpu | convert:host_to_gpu),
    staging (convert:gpu_to_gpu:host_staging), other.
  - D2H bytes (device->host), device->device peer bytes, same-device D2D bytes.
  - per-device H2D busy union (ms) and per-switch union ({0,1},{2,3}), any-GPU union.
  - per-copy rate stats for big copies (>=512KiB) by copyKind (1 vs 10) to check PtoP-labelled host copies run at line rate.
"""
import sqlite3, sys, csv, bisect
from collections import defaultdict

mapf, outf = sys.argv[1], sys.argv[2]
IT = sys.argv[3] if len(sys.argv) > 3 else "2"
rows = [r for r in csv.DictReader(open(mapf)) if r["iter"] == IT]

CATS = [("staging", "convert:gpu_to_gpu:host_staging"), ("scan_comp", "sirius::compression::host_to_gpu"),
        ("scan_raw", "convert:host_to_gpu"), ("peer", "convert:gpu_to_gpu:peer_copy"), ("g2g", "convert:gpu_to_gpu")]


def union(iv):
    iv.sort(); tot = 0; cs = ce = None
    for s, e in iv:
        if cs is None: cs, ce = s, e
        elif s > ce: tot += ce - cs; cs, ce = s, e
        else: ce = max(ce, e)
    if cs is not None: tot += ce - cs
    return tot


def analyze(db, w0, w1):
    c = sqlite3.connect(db)
    S = dict(c.execute("select id,value from StringIds"))
    # NVTX ranges of interest per thread
    rng = defaultdict(lambda: defaultdict(list))
    for s, e, tid, t, tidx in c.execute(
            "select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and end>=? and start<=?",
            (w0, w1)):
        n = t if t is not None else S.get(tidx, "")
        for cat, sub in CATS:
            if n.startswith(sub):
                rng[tid][cat].append((s, e)); break
    idx = {}
    for tid, d in rng.items():
        for cat, l in d.items():
            l.sort(); idx[(tid, cat)] = ([x[0] for x in l], l)

    def cat_of(tid, t):
        for cat, _ in CATS:
            k = (tid, cat)
            if k not in idx: continue
            st, l = idx[k]
            i = bisect.bisect_right(st, t) - 1
            # check a few preceding ranges (nesting of same name unlikely)
            while i >= 0 and i >= bisect.bisect_right(st, t) - 4:
                if l[i][0] <= t <= l[i][1]: return cat
                i -= 1
        return "other"

    q = """select m.start,m.end,m.deviceId,m.bytes,m.copyKind,m.srcKind,m.dstKind,m.srcDeviceId,r.globalTid,r.start
           from CUPTI_ACTIVITY_KIND_MEMCPY m left join CUPTI_ACTIVITY_KIND_RUNTIME r on r.correlationId=m.correlationId
           where m.start>=? and m.end<=?"""
    res = defaultdict(float)
    h2d_iv = defaultdict(list)
    rate = defaultdict(lambda: [0, 0, 0])  # kind -> [bytes, ns, n] for copies >=512KiB
    for s, e, dev, b, ck, sk, dk, sdev, tid, rs in c.execute(q, (w0, w1)):
        if sk in (0, 1) and dk == 2:
            cat = cat_of(tid, rs) if tid is not None else "other"
            if cat in ("g2g", "peer"): cat = "staging" if cat == "g2g" else cat
            res[f"h2d_{cat}"] += b; res["h2d"] += b; res[f"h2d_dev{dev}"] += b
            h2d_iv[dev].append((s, e))
            if b >= 512 * 1024:
                r = rate[ck]; r[0] += b; r[1] += e - s; r[2] += 1
        elif sk == 2 and dk in (0, 1):
            res["d2h"] += b
            cat = cat_of(tid, rs) if tid is not None else "other"
            res[f"d2h_{cat}"] += b
        elif sk == 2 and dk == 2:
            if ck == 10: res["p2p_dd"] += b
            else: res["d2d"] += b
    out = {k: v for k, v in res.items()}
    for d in range(4):
        out[f"busy_dev{d}_ms"] = union(list(h2d_iv.get(d, []))) / 1e6
    out["busy_sw0_ms"] = union(h2d_iv.get(0, []) + h2d_iv.get(1, [])) / 1e6
    out["busy_sw1_ms"] = union(h2d_iv.get(2, []) + h2d_iv.get(3, [])) / 1e6
    out["busy_any_ms"] = union([x for v in h2d_iv.values() for x in v]) / 1e6
    for ck, (b, ns, n) in rate.items():
        out[f"rate_k{ck}_GBps"] = b / ns if ns else 0  # bytes/ns == GB/s
        out[f"n_big_k{ck}"] = n
    return out


allrows = []
for r in rows:
    w0, w1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    o = analyze(r["sqlite"], w0, w1)
    o["query"] = int(r["query"]); o["wall_ms"] = (w1 - w0) / 1e6
    allrows.append(o)
    print(r["query"], f"wall {o['wall_ms']:.0f} ms h2d {o.get('h2d',0)/1e9:.1f} GB scan {o.get('h2d_scan_comp',0)/1e9+o.get('h2d_scan_raw',0)/1e9:.1f} stg {o.get('h2d_staging',0)/1e9:.2f}", flush=True)
keys = sorted({k for o in allrows for k in o})
keys.remove("query"); keys = ["query"] + keys
with open(outf, "w") as f:
    w = csv.DictWriter(f, keys); w.writeheader()
    for o in sorted(allrows, key=lambda x: x["query"]): w.writerow(o)
