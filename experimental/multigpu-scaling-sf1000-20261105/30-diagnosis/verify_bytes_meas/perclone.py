#!/usr/bin/env python3
"""perclone.py <map.csv> <iter> <out.csv>: one row per cross-GPU clone (g4 nsys window):
query,kind,dur_ms,MB,n_copies,n_sync,own_dma_ms,gap_ms(issued-not-started, no foreign same-dir copy),ce_queue_ms,host_ms,tail_ms,share_ms"""
import sqlite3, sys, csv, bisect
from collections import defaultdict
sys.path.insert(0, __file__.rsplit('/',1)[0])
mp, IT, outp = sys.argv[1], sys.argv[2], sys.argv[3]
def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out
class Busy:
    def __init__(s, iv): s.U = union(iv); s.S = [u[0] for u in s.U]
    def busy(s, a, b):
        i = max(bisect.bisect_right(s.S, a) - 1, 0); t = 0
        while i < len(s.U) and s.U[i][0] < b:
            t += max(0, min(b, s.U[i][1]) - max(a, s.U[i][0])); i += 1
        return t
rows = [r for r in csv.DictReader(open(mp)) if r["iter"] == IT]
W = csv.writer(open(outp, "w")); W.writerow("query,kind,dur_ms,MB,n_copies,n_sync,own_dma_ms,gap_ms,ce_queue_ms,host_ms,tail_ms,share_ms,consumer".split(","))
for r in rows:
    q = int(r["query"]); W0, W1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    c = sqlite3.connect(r["sqlite"]); S = dict(c.execute("select id,value from StringIds"))
    nv = [(s, e, tid, t if t is not None else S.get(tx, "")) for s, e, tid, t, tx in c.execute(
        "select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (W0, W1))]
    inner = defaultdict(list); tasks_by = defaultdict(list)
    for s, e, tid, n in nv:
        if n in ("convert:gpu_to_gpu:host_staging", "convert:gpu_to_gpu:peer_copy"):
            inner[tid].append((s, e, "st" if n.endswith("staging") else "pe"))
        if n.startswith("Pipeline ") and " Task " in n: tasks_by[tid].append((s, e, n))
    for v in inner.values(): v.sort()
    for v in tasks_by.values(): v.sort()
    tasks = [(s, e) for v in tasks_by.values() for s, e, _ in v]
    ev = sorted([(s, 1) for s, e in tasks] + [(e, -1) for s, e in tasks]); T = []; Nn = []; n = 0
    for t, d in ev:
        n += d
        if T and T[-1] == t: Nn[-1] = n
        else: T.append(t); Nn.append(n)
    def share(a, b):
        i = max(bisect.bisect_right(T, a) - 1, 0); acc = 0.0
        while i < len(T) and T[i] < b:
            x = max(a, T[i]); y = min(b, T[i + 1] if i + 1 < len(T) else b)
            if y > x: acc += (y - x) / max(Nn[i], 1)
            i += 1
        return acc
    names = dict(c.execute("select id,value from StringIds"))
    rt = defaultdict(list)
    for s, e, tid, cid, nid in c.execute("select start,end,globalTid,correlationId,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (W0, W1)):
        rt[tid].append((s, e, cid, names.get(nid, "")))
    for v in rt.values(): v.sort()
    mc = {}; bydir = defaultdict(list)
    for s, e, dev, sk, dk, b, cid in c.execute("select start,end,deviceId,srcKind,dstKind,bytes,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and start<=?", (W0, W1 + 10**9)):
        d = "pp" if (sk == 2 and dk == 2) else ("d2h" if sk == 2 else ("h2d" if dk == 2 else "hh"))
        mc[cid] = (s, e, dev, d, b); bydir[(dev, d)].append((s, e))
    BA = {k: Busy(v) for k, v in bydir.items()}
    for s, e, tid, nm in nv:
        if nm != "convert:gpu_to_gpu": continue
        L = inner.get(tid, []); i = bisect.bisect_left(L, (s, 0, ""))
        if not (i < len(L) and L[i][0] <= e): continue
        kind = L[i][2]
        TT = tasks_by.get(tid, []); k = bisect.bisect_right(TT, (s, 1 << 62, "")) - 1
        cons = TT[k][2].split(" Task")[0] if k >= 0 and TT[k][0] <= s <= TT[k][1] else ""
        R = rt.get(tid, []); j = bisect.bisect_left(R, (s, 0, 0, "")); own = []; nsync = 0
        while j < len(R) and R[j][0] < e:
            if "Synchronize" in R[j][3]: nsync += 1
            m = mc.get(R[j][2])
            if m: own.append((R[j][0],) + m)
            j += 1
        own.sort(key=lambda x: x[1]); cat = defaultdict(float); t = s
        byt = sum(x[5] for x in own if x[4] in ("pp", "d2h"))
        for iss, ms, me, dev, d, b in own:
            if ms > t:
                a = t
                if iss > a: cat["host"] += min(iss, ms) - a; a = min(iss, ms)
                if ms > a:
                    fb = BA[(dev, d)].busy(a, ms); cat["ce"] += fb; cat["gap"] += (ms - a) - fb
                t = ms
            if me > t: cat["dma"] += me - t; t = me
        if e > t: cat["tail"] += e - t
        W.writerow([q, kind, round((e - s) / 1e6, 4), round(byt / 1e6, 3), len(own), nsync, round(cat["dma"] / 1e6, 4), round(cat["gap"] / 1e6, 4),
                    round(cat["ce"] / 1e6, 4), round(cat["host"] / 1e6, 4), round(cat["tail"] / 1e6, 4), round(share(s, e) / 1e6, 4), cons])
    print(q, flush=True)
