#!/usr/bin/env python3
"""link_states.py <map.csv> <ngpu> [iter=2]

Per query window and per PCIe switch, split wall into:
  copy   : some host->device memcpy active on the switch's GPUs (link busy)
  prep_idle : a scan-prepare NVTX range (sirius::compression::host_to_gpu | convert:host_to_gpu) is open on
              some thread working for the switch, but no H2D copy is active (decode, allocator, API issue, syncs)
  none   : neither (non-scan phases / tails)
Thread->GPU of a prepare range = device of the memcpys it launched.
Also: sum of per-range wall split into time-with-own-copy-active vs not (per-task critical path view).
"""
import sqlite3, sys, csv, bisect
from collections import defaultdict, Counter

mapf, ng = sys.argv[1], int(sys.argv[2])
IT = sys.argv[3] if len(sys.argv) > 3 else "2"
SW = [[0, 1], [2, 3]] if ng == 4 else [[0]]
PREP = ("sirius::compression::host_to_gpu", "convert:host_to_gpu")


def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if e <= s: continue
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out


def L(u): return sum(e - s for s, e in u)


def inter(a, b):
    i = j = 0; out = []
    while i < len(a) and j < len(b):
        s = max(a[i][0], b[j][0]); e = min(a[i][1], b[j][1])
        if s < e: out.append([s, e])
        if a[i][1] < b[j][1]: i += 1
        else: j += 1
    return out


rows = [r for r in csv.DictReader(open(mapf)) if r["iter"] == IT]
w = csv.writer(sys.stdout)
w.writerow(["query", "wall_ms"] + [f"{k}_s{i}" for i in range(len(SW)) for k in ("copy", "prep_idle", "none")] +
           ["prep_ranges_ms", "prep_own_copy_ms", "n_prep"])
tot = Counter()
for r in rows:
    w0, w1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    c = sqlite3.connect(r["sqlite"])
    S = dict(c.execute("select id,value from StringIds"))
    prep = defaultdict(list)
    for s, e, tid, t, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (w0, w1)):
        n = t if t is not None else S.get(tx, "")
        if n == PREP[0] or n == PREP[1]:
            prep[tid].append((s, e))
    cp = defaultdict(list)  # dev -> copy intervals
    launched = defaultdict(list)  # tid -> (rt_start, dev, s, e)
    for s, e, dev, tid, rs in c.execute("""select m.start,m.end,m.deviceId,r.globalTid,r.start from CUPTI_ACTIVITY_KIND_MEMCPY m
            left join CUPTI_ACTIVITY_KIND_RUNTIME r on r.correlationId=m.correlationId
            where m.start>=? and m.end<=? and m.srcKind in (0,1) and m.dstKind=2""", (w0, w1)):
        cp[dev].append((s, e))
        if tid in prep: launched[tid].append((rs, dev, s, e))
    devr = defaultdict(list)  # dev -> prep intervals
    own_tot = 0; prep_tot = 0; nprep = 0
    for tid, rs in prep.items():
        ls = sorted(launched[tid]); st = [x[0] for x in ls]
        for s, e in rs:
            sub = ls[bisect.bisect_left(st, s):bisect.bisect_right(st, e)]
            if not sub: continue
            dev = Counter(x[1] for x in sub).most_common(1)[0][0]
            devr[dev].append((s, e)); nprep += 1
            prep_tot += e - s
            own_tot += L(inter(union([(a, b) for _, _, a, b in sub]), [[s, e]]))
    out = [r["query"], (w1 - w0) / 1e6]
    for gs in SW:
        cu = union([x for d in gs for x in cp[d]])
        pu = union([x for d in gs for x in devr[d]])
        both = union(cu + pu)
        out += [L(cu) / 1e6, (L(both) - L(cu)) / 1e6, (w1 - w0 - L(both)) / 1e6]
    out += [prep_tot / 1e6, own_tot / 1e6, nprep]
    w.writerow([out[0]] + [round(x, 1) for x in out[1:]])
    for i, x in enumerate(out[1:]): tot[i] += x
w.writerow(["TOT"] + [round(tot[i], 1) for i in range(len(tot))])
