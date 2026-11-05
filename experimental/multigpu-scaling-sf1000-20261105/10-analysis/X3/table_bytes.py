#!/usr/bin/env python3
"""table_bytes.py <run_dir_with_nsys_map_and_log> <quent_dir> [iter=2]

Wire bytes (nsys H2D launched inside 'Pipeline K: GPU_SCAN (id=X)' ranges) per (query, table) and decoded bytes
(Quent HOST-task in_bytes summed per GPU_SCAN(X) pipeline, mean over the same iter label) -> compression ratio.
Table of operator X comes from the run log line 'assigned pinned entry <t> to operator <X>' after the label call.
"""
import sqlite3, sys, csv, re, glob, bisect
from collections import defaultdict

run, qd = sys.argv[1], sys.argv[2]
IT = sys.argv[3] if len(sys.argv) > 3 else "2"
# --- op -> table from log
optab = defaultdict(dict); cur = None
for line in open(glob.glob(f"{run}/log/*.log")[0]):
    m = re.search(r"sirius_set_query_label\('tpch_q(\d+)_iter(\d+)'\)", line)
    if m: cur = (int(m.group(1)), m.group(2)); continue
    m = re.search(r"assigned pinned entry '(\w+)' to operator '(\d+)'", line)
    if m and cur: optab[cur][int(m.group(2))] = m.group(1)
# --- Quent decoded bytes per (query, op id)
pl = {r["pipeline"]: r["label"] for r in csv.DictReader(open(f"{qd}/pipelines.csv"))}
dec = defaultdict(float); ntask = defaultdict(int)
for r in csv.DictReader(open(f"{qd}/tasks.csv")):
    if r["origin"] != "HOST" or not r["query"].endswith(f"_iter{IT}"): continue
    m = re.match(r"GPU_SCAN\((\d+)\)", pl.get(r["pipeline"], ""))
    if not m: continue
    q = int(r["query"].split("_")[1][1:])
    dec[(q, int(m.group(1)))] += float(r["in_bytes"] or 0); ntask[(q, int(m.group(1)))] += 1
# --- nsys wire bytes
rows = [r for r in csv.DictReader(open(f"{run}/nsys/map.csv")) if r["iter"] == IT]
out = []
for r in rows:
    q = int(r["query"]); w0, w1 = int(r["range_start_ns"]), int(r["range_end_ns"])
    c = sqlite3.connect(r["sqlite"]); S = dict(c.execute("select id,value from StringIds"))
    rng = defaultdict(list); p2op = {}
    for s, e, tid, t, tx in c.execute("select start,end,globalTid,text,textId from NVTX_EVENTS where eventType=59 and end is not null and start>=? and end<=?", (w0, w1)):
        n = t if t is not None else S.get(tx, "")
        m = re.match(r"Pipeline (\d+): GPU_SCAN \(id=(\d+)\)$", n)
        if m: p2op[int(m.group(1))] = int(m.group(2)); continue
        m = re.match(r"Pipeline (\d+)( Task \d+.*)?$", n)
        if m: rng[tid].append((s, e, int(m.group(1))))
    idx = {t: (sorted(v), [x[0] for x in sorted(v)]) for t, v in rng.items()}
    wire = defaultdict(float)
    for b, tid, rs in c.execute("""select m.bytes,r.globalTid,r.start from CUPTI_ACTIVITY_KIND_MEMCPY m join CUPTI_ACTIVITY_KIND_RUNTIME r
            on r.correlationId=m.correlationId where m.start>=? and m.end<=? and m.srcKind in (0,1) and m.dstKind=2""", (w0, w1)):
        op = None
        if tid in idx:
            v, st = idx[tid]; i = bisect.bisect_right(st, rs) - 1
            for j in range(i, max(i - 4, -1), -1):
                if v[j][0] <= rs <= v[j][1]: op = p2op.get(v[j][2], ("pipe", v[j][2])); break
        wire[op] += b
    for op, b in wire.items():
        tab = optab.get((q, IT), {}).get(op, "?") if op is not None else "(non-scan)"
        d = dec.get((q, op), 0)
        out.append(dict(query=q, op=op, table=tab, wire_GB=b / 1e9, decoded_GB=d / 1e9, ratio=(d / b if b and d else ""), tasks=ntask.get((q, op), 0)))
w = csv.DictWriter(sys.stdout, out[0].keys()); w.writeheader()
for o in sorted(out, key=lambda x: (x["query"], str(x["op"]))): w.writerow(o)
