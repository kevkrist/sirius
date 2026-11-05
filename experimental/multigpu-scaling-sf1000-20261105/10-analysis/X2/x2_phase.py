#!/usr/bin/env python3
"""x2_phase.py <map.csv> [iter]: per query, union wall of clone ranges, staged/peer bytes, aggregate GB/s,
hardware floor (per-switch downstream staged bytes / 52 GB/s; intra P2P / 53.7 GB/s per directed pair), scan-H2D active union."""
import sqlite3, sys, csv, bisect
from collections import Counter, defaultdict
mp = sys.argv[1]; IT = sys.argv[2] if len(sys.argv) > 2 else "2"
SW = lambda d: 0 if d in (0, 1) else 1
def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out
L = lambda U: sum(e - s for s, e in U) / 1e6
print("q,wall,clone_union_ms,stage_union_ms,stage_GB,peer_GB,agg_GBps,floor_ms,dn_sw0_GB,dn_sw1_GB,maxpair_peer_GB,scan_h2d_union_ms,clone_scan_overlap_ms,first_clone_ms,last_scan_h2d_ms")
for r in csv.DictReader(open(mp)):
    if r["iter"] != IT: continue
    W0, W1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    c = sqlite3.connect(r["sqlite"]); S = dict(c.execute("select id,value from StringIds"))
    nv = [(s, e, tid, t if t is not None else S.get(tx, "")) for s, e, tid, t, tx in c.execute(
        "select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (W0, W1))]
    st = [x for x in nv if x[3] == "convert:gpu_to_gpu:host_staging"]; pc = [x for x in nv if x[3] == "convert:gpu_to_gpu:peer_copy"]
    allc = [x for x in nv if x[3] == "convert:gpu_to_gpu"]
    tids = {x[2] for x in allc}
    # memcpys by correlation launched inside clone ranges
    rt = defaultdict(list)
    for s, e, tid, corr in c.execute("select start,end,globalTid,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (W0, W1)):
        if tid in tids: rt[tid].append((s, corr))
    for v in rt.values(): v.sort()
    clone_corr = {}
    for kind, R in (("st", st), ("pc", pc)):
        for s, e, tid, _ in R:
            V = rt[tid]
            for _, corr in V[bisect.bisect_left(V, (s, -1)):bisect.bisect_right(V, (e, 1 << 62))]: clone_corr[corr] = kind
    dn = Counter(); pairp = Counter(); sgb = 0; pgb = 0; scan = []
    for ms, me, corr, b, sk, dk, dev, sd, dd in c.execute("select start,end,correlationId,bytes,srcKind,dstKind,deviceId,srcDeviceId,dstDeviceId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?", (W0, W1)):
        k = clone_corr.get(corr)
        d = dev if dd is None else dd
        if k == "st" and sk == 1 and dk == 2: dn[SW(d)] += b; sgb += b
        elif k == "pc": pairp[(sd, dd)] += b; pgb += b
        elif k is None and sk == 1 and dk == 2 and b > 65536: scan.append((ms, me))
    Uc = union([(s, e) for s, e, _, _ in allc]); Us = union([(s, e) for s, e, _, _ in st]); Uh = union(scan)
    ov = 0
    for a, b in Uc:
        for x, y in Uh:
            ov += max(0, min(b, y) - max(a, x))
    floor = max(max(dn.values(), default=0) / 52e9, max(pairp.values(), default=0) / 53.7e9) * 1e3
    fc = (min((s for s, e in Uc), default=W0) - W0) / 1e6; ls = (max((e for s, e in Uh), default=W0) - W0) / 1e6
    print(f"{r['query']},{(W1-W0)/1e6:.0f},{L(Uc):.1f},{L(Us):.1f},{sgb/1e9:.2f},{pgb/1e9:.2f},{(sgb+pgb)/max(1,L(Uc)*1e6):.1f},{floor:.1f},{dn[0]/1e9:.2f},{dn[1]/1e9:.2f},{max(pairp.values(),default=0)/1e9:.2f},{L(Uh):.0f},{ov/1e6:.1f},{fc:.0f},{ls:.0f}")
