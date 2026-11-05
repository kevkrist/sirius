#!/usr/bin/env python3
"""stage_breakdown.py <map.csv> <iter> [queries]
Per staged (host_staging) and peer clone: split its outer convert:gpu_to_gpu time into
  own_dma   : one of the clone's own memcpys is executing
  ce_queue  : next own memcpy already issued but not started, while the device executing it has
              another (foreign) memcpy of the same direction running  -> copy-engine/link contention
  gpu_wait  : issued, not started, no foreign same-direction copy running (stream dependency
              e.g. wait_for_writer event, or driver latency)
  host      : next own memcpy not yet issued (CPU: sync return latency, alloc, loop, cast kernels)
  tail      : after the last own memcpy ended (final sync return, release)
Also accumulates 1/n_active wall-share weights per category.
"""
import sqlite3, sys, csv, bisect
from collections import defaultdict

mp, IT = sys.argv[1], sys.argv[2]
QS = set(sys.argv[3].split(",")) if len(sys.argv) > 3 else None


def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out


class Busy:
    def __init__(self, iv):
        self.U = union(iv); self.S = [u[0] for u in self.U]

    def busy(self, a, b):
        U = self.U; i = max(bisect.bisect_right(self.S, a) - 1, 0); t = 0
        while i < len(U) and U[i][0] < b:
            t += max(0, min(b, U[i][1]) - max(a, U[i][0])); i += 1
        return t


rows = [r for r in csv.DictReader(open(mp)) if r["iter"] == IT and (QS is None or r["query"] in QS)]
rows.sort(key=lambda r: int(r["query"]))
TOT = defaultdict(float)
print("q kind n thread_ms own_dma ce_queue gpu_wait host tail | share_ms")
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
    ev = sorted([(s, 1) for s, e, _, _ in tasks] + [(e, -1) for s, e, _, _ in tasks])
    T = []; Nn = []; n = 0
    for t, d in ev:
        n += d
        if T and T[-1] == t: Nn[-1] = n
        else: T.append(t); Nn.append(n)

    def wfac(a, b):  # mean 1/n_active over [a,b]
        if b <= a: return 0.0
        i = max(bisect.bisect_right(T, a) - 1, 0); acc = 0.0
        while i < len(T) and T[i] < b:
            x = max(a, T[i]); y = min(b, T[i + 1] if i + 1 < len(T) else b)
            if y > x: acc += (y - x) / max(Nn[i], 1)
            i += 1
        return acc / (b - a)
    rt = defaultdict(list)
    for s, e, tid, cid in c.execute(
            "select start,end,globalTid,correlationId from CUPTI_ACTIVITY_KIND_RUNTIME where start>=? and end<=?", (W0, W1)):
        rt[tid].append((s, e, cid))
    for v in rt.values(): v.sort()
    mc = {}; bydir = defaultdict(list)
    for s, e, dev, sk, dk, cid in c.execute(
            "select start,end,deviceId,srcKind,dstKind,correlationId from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and start<=?",
            (W0, W1 + 1_000_000_000)):
        d = "pp" if (sk == 2 and dk == 2) else ("d2h" if sk == 2 else ("h2d" if dk == 2 else "hh"))
        mc[cid] = (s, e, dev, d); bydir[(dev, d)].append((s, e, cid))
    busy_all = {k: Busy([(s, e) for s, e, _ in v]) for k, v in bydir.items()}
    agg = defaultdict(float)
    for s, e, tid, _ in outer:
        L = inner.get(tid, []); i = bisect.bisect_left(L, (s, 0, ""))
        if not (i < len(L) and L[i][0] <= e): continue
        kind = L[i][2]
        R = rt.get(tid, []); j = bisect.bisect_left(R, (s, 0, 0))
        own = []
        while j < len(R) and R[j][0] < e:
            m = mc.get(R[j][2])
            if m: own.append((R[j][0], m[0], m[1], m[2], m[3]))  # issue, start, end, dev, dir
            j += 1
        own.sort(key=lambda x: x[1])
        cat = defaultdict(float); t = s
        for k, (iss, ms, me, dev, d) in enumerate(own):
            # time t..ms before this copy starts
            if ms > t:
                a = t
                if iss > a:
                    cat["host"] += min(iss, ms) - a; a = min(iss, ms)
                if ms > a:
                    fb = busy_all[(dev, d)].busy(a, ms)  # includes own? own not started yet -> foreign
                    cat["ce_queue"] += fb; cat["gpu_wait"] += (ms - a) - fb
                t = ms
            if me > t:
                cat["own_dma"] += me - t; t = me
        if e > t: cat["tail"] += e - t
        wf = wfac(s, e)
        agg[kind + "_n"] += 1; agg[kind + "_ms"] += (e - s) / 1e6
        for k2, v in cat.items():
            agg[kind + "_" + k2] += v / 1e6; agg[kind + "_" + k2 + "_w"] += v * wf / 1e6
        agg[kind + "_w"] += (e - s) * wf / 1e6
    for kind in ("st", "pe"):
        print(q, kind, int(agg[kind + "_n"]), round(agg[kind + "_ms"]),
              *[round(agg[kind + "_" + k]) for k in ("own_dma", "ce_queue", "gpu_wait", "host", "tail")],
              "|", round(agg[kind + "_w"]), [round(agg[kind + "_" + k + "_w"]) for k in ("own_dma", "ce_queue", "gpu_wait", "host", "tail")])
    for k, v in agg.items(): TOT[k] += v
for kind in ("st", "pe"):
    print("TOTAL", kind, int(TOT[kind + "_n"]), round(TOT[kind + "_ms"]),
          *[round(TOT[kind + "_" + k]) for k in ("own_dma", "ce_queue", "gpu_wait", "host", "tail")],
          "|", round(TOT[kind + "_w"]), [round(TOT[kind + "_" + k + "_w"]) for k in ("own_dma", "ce_queue", "gpu_wait", "host", "tail")])
