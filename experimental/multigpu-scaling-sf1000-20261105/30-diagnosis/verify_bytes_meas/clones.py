#!/usr/bin/env python3
"""clones.py <map.csv> <iter> <out.csv>
Independent re-derivation of cross-GPU clone accounting from nsys (window-restricted).
Per query: staged/peer clone count, payload bytes, thread-ms (outer convert:gpu_to_gpu range),
leg DMA busy (D2H, H2D per clone; union of own memcpys), sync ms inside clones,
1/n_active wall share of clones, per-switch staged H2D rate during staged-clone union,
and a 'perfect leg overlap' thread-ms lever = sum over staged clones of min(D2H busy, H2D busy).
"""
import sqlite3, sys, csv, bisect
from collections import defaultdict

mp, IT, outp = sys.argv[1], sys.argv[2], sys.argv[3]
SW = lambda d: 0 if d in (0, 1) else 1


def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out


def ulen(iv):
    return sum(e - s for s, e in union(iv))


rows = [r for r in csv.DictReader(open(mp)) if r["iter"] == IT]
rows.sort(key=lambda r: int(r["query"]))
out = []
for r in rows:
    q = int(r["query"]); W0, W1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    c = sqlite3.connect(r["sqlite"]); S = dict(c.execute("select id,value from StringIds"))
    nv = []
    for s, e, tid, t, tx in c.execute(
            "select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?",
            (W0, W1)):
        nv.append((s, e, tid, t if t is not None else S.get(tx, "")))
    outer = [x for x in nv if x[3] == "convert:gpu_to_gpu"]
    inner = defaultdict(list)
    for s, e, tid, n in nv:
        if n in ("convert:gpu_to_gpu:host_staging", "convert:gpu_to_gpu:peer_copy"):
            inner[tid].append((s, e, "st" if n.endswith("staging") else "pe"))
    for v in inner.values(): v.sort()
    tasks = [x for x in nv if x[3].startswith("Pipeline ") and " Task " in x[3]]
    # n_active step function for wall share
    ev = sorted([(s, 1) for s, e, _, _ in tasks] + [(e, -1) for s, e, _, _ in tasks])
    T = []; Nn = []; n = 0
    for t, d in ev:
        n += d
        if T and T[-1] == t: Nn[-1] = n
        else: T.append(t); Nn.append(n)

    def share(s, e):
        i = max(bisect.bisect_right(T, s) - 1, 0); acc = 0.0
        while i < len(T) and T[i] < e:
            a = max(s, T[i]); b = min(e, T[i + 1] if i + 1 < len(T) else e)
            k = Nn[i] if T[i] <= s or True else 0
            if b > a: acc += (b - a) / max(Nn[i], 1)
            i += 1
        return acc
    # runtime calls by tid
    names = dict(c.execute("select id,value from StringIds"))
    rt = defaultdict(list)
    for s, e, tid, cid, nid in c.execute(
            "select start,end,globalTid,correlationId,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?",
            (W0, W1)):
        rt[tid].append((s, e, cid, names.get(nid, "")))
    for v in rt.values(): v.sort()
    mc = {}
    for s, e, dev, b, ck, sk, dk, sd, dd, cid in c.execute(
            "select start,end,deviceId,bytes,copyKind,srcKind,dstKind,srcDeviceId,dstDeviceId,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and start<=?",
            (W0, W1 + 2_000_000_000)):
        mc[cid] = (s, e, dev, b, ck, sk, dk, sd, dd)
    agg = defaultdict(float)
    st_iv = []; h2d_by_sw = defaultdict(list)  # staged H2D memcpys (s,e,bytes) per dest switch
    d2h_by_sw = defaultdict(list)
    for s, e, tid, _ in outer:
        L = inner.get(tid, []); i = bisect.bisect_left(L, (s, 0, ""))
        kind = None
        if i < len(L) and L[i][0] <= e: kind = L[i][2]
        if kind is None: continue  # same-device clone
        R = rt.get(tid, []); j = bisect.bisect_left(R, (s, 0, 0, ""))
        d2h = []; h2d = []; pp = []; byt = 0; syn = 0; nsync = 0
        while j < len(R) and R[j][0] < e:
            rs, re_, cid, nm = R[j]
            if "Synchronize" in nm: syn += re_ - rs; nsync += 1
            m = mc.get(cid)
            if m:
                ms, me, dev, b, ck, sk, dk, sd, dd = m
                if sk == 2 and dk == 2:  # device->device (peer)
                    pp.append((ms, me)); byt += b
                elif sk == 2:  # device -> host (D2H leg)
                    d2h.append((ms, me)); byt += b
                    d2h_by_sw[SW(dev)].append((ms, me, b))
                elif dk == 2:  # host -> device (H2D leg)
                    h2d.append((ms, me))
                    h2d_by_sw[SW(dev)].append((ms, me, b))
            j += 1
        dur = e - s
        agg[kind + "_n"] += 1; agg[kind + "_ms"] += dur / 1e6; agg[kind + "_GB"] += byt / 1e9
        agg[kind + "_sync_ms"] += syn / 1e6; agg[kind + "_nsync"] += nsync
        agg[kind + "_share_ms"] += share(s, e) / 1e6
        if kind == "st":
            dl, hl = ulen(d2h), ulen(h2d)
            agg["st_d2h_ms"] += dl / 1e6; agg["st_h2d_ms"] += hl / 1e6
            agg["st_overlap_lever_ms"] += min(dl, hl) / 1e6
            # share-weighted lever: lever * (share/dur)
            agg["st_overlap_lever_share_ms"] += (min(dl, hl) / dur * share(s, e) / 1e6) if dur > 0 else 0
            st_iv.append((s, e))
        else:
            agg["pe_dma_ms"] += ulen(pp) / 1e6
    U = union(st_iv); ulen_st = sum(e - s for s, e in U)
    # per-switch staged H2D GB/s during staged union (bytes / union time) -- rate while staging active
    for sw in (0, 1):
        b = sum(x[2] for x in h2d_by_sw[sw]); busy = ulen([(x[0], x[1]) for x in h2d_by_sw[sw]])
        agg[f"sw{sw}_st_h2d_GB"] = b / 1e9
        agg[f"sw{sw}_st_h2d_GBps_union"] = (b / ulen_st) if ulen_st else 0  # bytes/ns == GB/s
        agg[f"sw{sw}_st_h2d_GBps_busy"] = (b / busy) if busy else 0
    agg["st_union_ms"] = ulen_st / 1e6
    agg["query"] = q; agg["window_ms"] = (W1 - W0) / 1e6
    out.append(dict(agg))
    print(q, {k: round(v, 1) for k, v in agg.items() if k in ("st_n", "st_GB", "st_ms", "st_share_ms", "pe_n", "pe_GB", "pe_ms", "pe_share_ms", "st_overlap_lever_share_ms", "st_union_ms", "sw0_st_h2d_GBps_union", "sw1_st_h2d_GBps_union")}, flush=True)
keys = sorted({k for d in out for k in d})
with open(outp, "w") as f:
    w = csv.DictWriter(f, fieldnames=keys); w.writeheader()
    for d in out: w.writerow(d)
tot = defaultdict(float)
for d in out:
    for k, v in d.items():
        if k != "query": tot[k] += v
print("TOTAL", {k: round(v, 1) for k, v in sorted(tot.items())})
