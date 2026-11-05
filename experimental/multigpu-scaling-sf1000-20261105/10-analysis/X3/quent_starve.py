#!/usr/bin/env python3
"""quent_starve.py <quent_dir> <ngpu> [iters=2,3,4]

Per query (mean over hot iters), per PCIe switch (g4: {0,1},{2,3}; g1: {0}):
  wall        query wall (queries.csv)
  prep_on     union of HOST-origin Preparing intervals [preparing, first_compute) on the switch's GPUs (link+decode busy proxy)
  pend        union of intervals where a HOST-origin task bound to the switch's GPUs is created but not yet picked up [created, routed)
  starve      pend AND NOT prep_on  -> the link is idle although scan work for it is queued (all executor threads busy elsewhere)
  scan_span   first HOST task created -> last HOST task finalize
  host_prep_ms / host_cmp_ms  summed HOST-task thread-time in prepare / compute (first_compute->finalize)
  conc_prep   mean # HOST tasks in Preparing on the switch while prep_on (time-weighted)
"""
import csv, sys
from collections import defaultdict

qd, ng = sys.argv[1], int(sys.argv[2])
ITS = sys.argv[3].split(",") if len(sys.argv) > 3 else ["2", "3", "4"]
SW = [[0, 1], [2, 3]] if ng == 4 else ([[0]] if ng == 1 else [[g] for g in range(ng)])


def union(iv):
    iv = sorted(iv); out = []
    for s, e in iv:
        if e <= s: continue
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out


def length(u): return sum(e - s for s, e in u)


def minus(a, b):  # a \ b, both unions
    out = []; j = 0
    for s, e in a:
        cur = s
        while j < len(b) and b[j][1] <= cur: j += 1
        k = j
        while k < len(b) and b[k][0] < e:
            if b[k][0] > cur: out.append([cur, b[k][0]])
            cur = max(cur, b[k][1]); k += 1
        if cur < e: out.append([cur, e])
    return out


def conc(iv):
    ev = []
    for s, e in iv: ev += [(s, 1), (e, -1)]
    ev.sort(); n = 0; last = None; area = 0; on = 0
    for t, d in ev:
        if last is not None and n > 0: area += n * (t - last); on += t - last
        n += d; last = t
    return area / on if on else 0


Q = {r["query"]: float(r["wall_ms"]) for r in csv.DictReader(open(f"{qd}/queries.csv")) if r["wall_ms"]}
T = defaultdict(list)
for r in csv.DictReader(open(f"{qd}/tasks.csv")):
    T[r["query"]].append(r)
res = defaultdict(lambda: defaultdict(float))
for lab, ts in T.items():
    q, it = lab.split("_")[1][1:], lab.split("iter")[1]
    if it not in ITS: continue
    q = int(q)
    host = [t for t in ts if t["origin"] == "HOST" and t["preparing"] and t["first_compute"]]
    if not host: continue
    acc = res[q]; acc["n"] += 1 / len(ITS); acc["wall"] += Q[lab] / len(ITS)
    acc["scan_span"] += (max(int(t["finalize"] or t["first_compute"]) for t in host) - min(int(t["created"]) for t in host)) / 1e6 / len(ITS)
    acc["host_prep_ms"] += sum(int(t["first_compute"]) - int(t["preparing"]) for t in host) / 1e6 / len(ITS)
    acc["host_cmp_ms"] += sum(int(t["finalize"]) - int(t["first_compute"]) for t in host if t["finalize"]) / 1e6 / len(ITS)
    acc["n_host"] += len(host) / len(ITS)
    for si, gs in enumerate(SW):
        gs = [str(g) for g in gs]
        h = [t for t in host if t["gpu"] in gs]
        prep = [(int(t["preparing"]), int(t["first_compute"])) for t in h]
        pu = union(prep)
        pend = union([(int(t["created"]), int(t["routed"])) for t in h if t["routed"]])
        st = minus(pend, pu)
        acc[f"prep_on_s{si}"] += length(pu) / 1e6 / len(ITS)
        acc[f"pend_s{si}"] += length(pend) / 1e6 / len(ITS)
        acc[f"starve_s{si}"] += length(st) / 1e6 / len(ITS)
        acc[f"conc_s{si}"] += conc(prep) / len(ITS)
keys = ["wall", "scan_span", "n_host", "host_prep_ms", "host_cmp_ms"] + [f"{k}_s{i}" for i in range(len(SW)) for k in ("prep_on", "pend", "starve", "conc")]
w = csv.writer(sys.stdout); w.writerow(["query"] + keys)
tot = defaultdict(float)
for q in sorted(res):
    w.writerow([q] + [round(res[q][k], 2) for k in keys])
    for k in keys: tot[k] += res[q][k]
w.writerow(["TOT"] + [round(tot[k], 1) for k in keys])
