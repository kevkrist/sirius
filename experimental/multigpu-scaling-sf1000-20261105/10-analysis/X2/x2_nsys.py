#!/usr/bin/env python3
"""x2_nsys.py <map.csv> <out_prefix> [iter=2] [queries comma]
Per query window: cross-GPU clone accounting (convert:gpu_to_gpu:{peer_copy,host_staging}), scan H2D,
PARTITION/CONCAT work, and 'wall share' = integral of 1/n_active_tasks over the range (n_active = open
'Pipeline K Task M' NVTX ranges across all threads)."""
import sqlite3, sys, csv, re, bisect, json
from collections import defaultdict, Counter
mp, outp = sys.argv[1], sys.argv[2]
IT = sys.argv[3] if len(sys.argv) > 3 else "2"
QS = set(sys.argv[4].split(",")) if len(sys.argv) > 4 else None
SW = lambda d: 0 if d in (0, 1) else 1

def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out

def overlap(s, e, U, Us):
    # U union intervals sorted, Us starts
    i = max(bisect.bisect_right(Us, s) - 1, 0); t = 0
    while i < len(U) and U[i][0] < e:
        t += max(0, min(e, U[i][1]) - max(s, U[i][0])); i += 1
    return t

rows = [r for r in csv.DictReader(open(mp)) if r["iter"] == IT and (QS is None or r["query"] in QS)]
summ = []; per_task = []; pair = Counter(); pairb = Counter()
for r in rows:
    q = int(r["query"]); W0, W1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    c = sqlite3.connect(r["sqlite"]); S = dict(c.execute("select id,value from StringIds"))
    nv = []
    for s, e, tid, t, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (W0, W1)):
        nv.append((s, e, tid, t if t is not None else S.get(tx, "")))
    tasks = [x for x in nv if x[3].startswith("Pipeline ") and " Task " in x[3]]
    # n_active cumulative integral
    ev = sorted([(s, 1) for s, e, _, _ in tasks] + [(e, -1) for s, e, _, _ in tasks])
    T = [W0]; F = [0.0]; n = 0; last = W0; acc = 0.0; N = [0]
    for t, d in ev:
        if t > last:
            acc += (t - last) / n if n > 0 else 0.0
            T.append(t); F.append(acc); N.append(n); last = t
        n += d
        N[-1] = n if T[-1] == t else N[-1]
    def Fat(t):
        i = bisect.bisect_right(T, t) - 1
        if i < 0: return 0.0
        if i + 1 < len(T):
            # integrand on [T[i],T[i+1]] constant
            seg = (F[i + 1] - F[i]) / (T[i + 1] - T[i]) if T[i + 1] > T[i] else 0
            return F[i] + seg * (t - T[i])
        return F[i]
    share = lambda s, e: (Fat(e) - Fat(s)) / 1e6
    task_by_tid = defaultdict(list)
    for s, e, tid, nm in tasks: task_by_tid[tid].append((s, e, nm))
    for v in task_by_tid.values(): v.sort()
    def enclosing(tid, s):
        L = task_by_tid.get(tid, []); i = bisect.bisect_right(L, (s, 1 << 62, "")) - 1
        while i >= 0:
            if L[i][0] <= s <= L[i][1]: return L[i][2]
            i -= 1
        return ""
    conv = [x for x in nv if x[3] in ("convert:gpu_to_gpu:peer_copy", "convert:gpu_to_gpu:host_staging")]
    convall = [x for x in nv if x[3] == "convert:gpu_to_gpu"]
    tids = {x[2] for x in conv} | {x[2] for x in convall}
    rt = defaultdict(list)
    for s, e, tid, nid, corr in c.execute("select start,end,globalTid,nameId,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (W0, W1)):
        if tid in tids: rt[tid].append((s, e, S.get(nid, str(nid)), corr))
    for v in rt.values(): v.sort()
    mem = {}; h2d_scan = defaultdict(list); h2d_bytes_all = Counter()
    for s, e, corr, b, sk, dk, dev, sd, dd in c.execute("select start,end,correlationId,bytes,srcKind,dstKind,deviceId,srcDeviceId,dstDeviceId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=?", (W0, W1)):
        mem[corr] = (s, e, b, sk, dk, dev, sd, dd)
    staged_corr = set()
    recs = []
    for s, e, tid, nm in conv:
        calls = rt[tid]; i0 = bisect.bisect_left(calls, (s,)); i1 = bisect.bisect_right(calls, (e, 1 << 62))
        nsync = 0; tsync = 0; qlat = 0; dma = 0; legs = Counter(); legt = Counter(); devs = set(); h2d_iv = []; ncpy = 0; dst = None; src = None
        for cs, ce, cn, corr in calls[i0:i1]:
            if "StreamSynchronize" in cn: nsync += 1; tsync += ce - cs
            m = mem.get(corr)
            if m:
                ms, me, b, sk, dk, dev, sd, dd = m; ncpy += 1; staged_corr.add(corr); qlat += max(0, ms - ce); dma += me - ms
                if sk == 2 and dk == 1: legs["d2h"] += b; legt["d2h"] += me - ms; src = dev if sd is None else sd
                elif sk == 1 and dk == 2: legs["h2d"] += b; legt["h2d"] += me - ms; dst = dev if dd is None else dd; h2d_iv.append((ms, me, dst))
                elif sk == 2 and dk == 2: legs["p2p"] += b; legt["p2p"] += me - ms; src, dst = sd, dd
                else: legs["other"] += b
        enc = enclosing(tid, s); op = re.search(r"\[(\w+)", enc); op = op.group(1) if op else "?"
        recs.append(dict(s=s, e=e, kind="staging" if nm.endswith("staging") else "peer", op=op, enc=enc, nsync=nsync, tsync=tsync,
                         bytes=legs["p2p"] + legs["h2d"], d2h_ns=legt["d2h"], h2d_ns=legt["h2d"], p2p_ns=legt["p2p"],
                         src=src, dst=dst, ncpy=ncpy, qlat=qlat, dma=dma, h2d_iv=h2d_iv, share=share(s, e)))
    # scan H2D = pinned->device memcpys not in clone ranges
    for corr, (ms, me, b, sk, dk, dev, sd, dd) in mem.items():
        if sk == 1 and dk == 2 and corr not in staged_corr:
            d = dev if dd is None else dd
            h2d_scan[SW(d)].append((ms, me)); h2d_bytes_all[d] += b
    U = {k: union(v) for k, v in h2d_scan.items()}; Us = {k: [x[0] for x in v] for k, v in U.items()}
    ov = 0; tot = 0
    for x in recs:
        for ms, me, d in x["h2d_iv"]:
            tot += me - ms; ov += overlap(ms, me, U.get(SW(d), []), Us.get(SW(d), []))
    # partition / concat
    def rng(name):
        L = [x for x in nv if x[3] == name]
        return len(L), sum(e - s for s, e, _, _ in L) / 1e6, sum(share(s, e) for s, e, _, _ in L)
    hp = [x for x in nv if x[3] == "hash_partition"]
    pexec = [x for x in nv if x[3] == "sirius_physical_partition::execute"]
    hp_by = defaultdict(list)
    for s, e, tid, _ in hp: hp_by[tid].append(s)
    pe_hash = [x for x in pexec if any(x[0] <= t <= x[1] for t in hp_by.get(x[2], []))]
    agg = defaultdict(lambda: Counter())
    for x in recs:
        k = (x["kind"], x["op"]); a = agg[k]
        a["n"] += 1; a["ms"] += (x["e"] - x["s"]) / 1e6; a["share"] += x["share"]; a["bytes"] += x["bytes"]; a["nsync"] += x["nsync"]
        a["sync_ms"] += x["tsync"] / 1e6; a["d2h_ms"] += x["d2h_ns"] / 1e6; a["h2d_ms"] += x["h2d_ns"] / 1e6; a["p2p_ms"] += x["p2p_ns"] / 1e6
        a["qlat_ms"] += x["qlat"] / 1e6; a["dma_ms"] += x["dma"] / 1e6; a["ncpy"] += x["ncpy"]
        a["cross"] += 1 if (x["src"] is not None and x["dst"] is not None and SW(x["src"]) != SW(x["dst"])) else 0
        if x["src"] is not None and x["dst"] is not None:
            pair[(x["src"], x["dst"])] += 1; pairb[(x["src"], x["dst"])] += x["bytes"]
    for (kind, op), a in agg.items():
        per_task.append(dict(query=q, kind=kind, op=op, **{k: round(v, 3) for k, v in a.items()}))
    st = [x for x in recs if x["kind"] == "staging"]; pc = [x for x in recs if x["kind"] == "peer"]
    wall = (W1 - W0) / 1e6
    n1, ms1, sh1 = rng("sirius_physical_partition::execute"); n2, ms2, sh2 = rng("sirius_physical_concat::execute")
    summ.append(dict(query=q, wall_ms=round(wall, 1), task_ranges=len(tasks), busy_wall_ms=round(sum(T[i+1]-T[i] for i in range(len(T)-1) if F[i+1]>F[i]) / 1e6, 1),
        same_dev_clones=len(convall) - len(conv),
        n_stage=len(st), stage_ms=round(sum(x["e"] - x["s"] for x in st) / 1e6, 1), stage_GB=round(sum(x["bytes"] for x in st) / 1e9, 3),
        stage_share_ms=round(sum(x["share"] for x in st), 1), stage_syncs=sum(x["nsync"] for x in st), stage_sync_ms=round(sum(x["tsync"] for x in st)/1e6,1), stage_dma_ms=round(sum(x["dma"] for x in st)/1e6,1), stage_qlat_ms=round(sum(x["qlat"] for x in st)/1e6,1), outer_ms=round(sum(e-s for s,e,_,_ in convall)/1e6,1), outer_share_ms=round(sum(share(s,e) for s,e,_,_ in convall),1),
        n_peer=len(pc), peer_ms=round(sum(x["e"] - x["s"] for x in pc) / 1e6, 1), peer_GB=round(sum(x["bytes"] for x in pc) / 1e9, 3),
        peer_share_ms=round(sum(x["share"] for x in pc), 1), peer_syncs=sum(x["nsync"] for x in pc), peer_dma_ms=round(sum(x["dma"] for x in pc)/1e6,1),
        scan_h2d_GB=round(sum(h2d_bytes_all.values()) / 1e9, 2),
        stage_h2d_overlap_frac=round(ov / tot, 3) if tot else None,
        stage_GBps=round(sum(x["bytes"] for x in st) / max(1, sum(x["e"] - x["s"] for x in st)), 2),
        stage_leg_GBps=round(sum(x["bytes"] for x in st) / max(1, sum(x["h2d_ns"] for x in st)), 2),
        peer_GBps=round(sum(x["bytes"] for x in pc) / max(1, sum(x["e"] - x["s"] for x in pc)), 2),
        part_exec_n=n1, part_exec_ms=round(ms1, 1), part_exec_share=round(sh1, 1), part_hash_n=len(pe_hash),
        part_hash_ms=round(sum(e - s for s, e, _, _ in pe_hash) / 1e6, 1), part_hash_share=round(sum(share(s, e) for s, e, _, _ in pe_hash), 1),
        concat_n=n2, concat_ms=round(ms2, 1), concat_share=round(sh2, 1)))
    print(json.dumps(summ[-1]), flush=True)
for name, data in (("summary", summ), ("by_op", per_task)):
    keys = list(data[0].keys()) if name == "summary" else sorted({k for d in data for k in d}, key=lambda k: (k not in ("query", "kind", "op"), k))
    with open(f"{outp}_{name}.csv", "w") as fh:
        w = csv.DictWriter(fh, keys); w.writeheader(); [w.writerow(d) for d in data]
with open(f"{outp}_pairs.csv", "w") as fh:
    fh.write("src,dst,clones,GB\n")
    for k in sorted(pair): fh.write(f"{k[0]},{k[1]},{pair[k]},{pairb[k]/1e9:.3f}\n")
