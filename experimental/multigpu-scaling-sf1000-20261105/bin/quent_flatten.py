#!/usr/bin/env python3
"""Flatten a Sirius Quent ndjson telemetry directory into analysis tables.

usage: quent_flatten.py <telemetry_dir> <out_dir>

Writes:
  tasks.csv     one row per task: query label, pipeline id/label, executing GPU, timestamps (ns, epoch)
                for created/routed/reserving/preparing/first-compute/finalize, origin tier, input bytes,
                requested reservation bytes, preferred_device_id, success flag.
  task_ops.csv  one row per (task, operator) Computing phase: start/end ns, operator id, input bytes.
  queries.csv   query label, init/executing/finished ns, wall ms.
  pipelines.csv pipeline uuid, query label, label (operator chain).
  per_query_gpu.csv  per (query label, gpu): task count, busy ms (union of preparing->finalize
                intervals across that GPU's executor threads), sum of task ms, prepare ms (preparing->first
                compute; includes HOST->GPU upload / decompression), H2D bytes (input_bytes for origin HOST),
                first start / last end offsets from query start (ms).
Timestamps: ns since epoch (steady across files of one process).
"""
import csv, glob, json, os, sys
from collections import defaultdict

tdir, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
roots = [d for d in glob.glob(os.path.join(tdir, "*")) if os.path.isdir(d)]


def events(root, kind):
    for f in glob.glob(os.path.join(root, kind, "*.ndjson")):
        with open(f) as fh:
            for line in fh:
                line = line.strip()
                if line:
                    yield json.loads(line)


def tgt(x):
    return x["target"] if isinstance(x, dict) else None


qrows, prow, trows, orows = [], [], [], []
for root in roots:
    threads = {}  # executor thread id -> gpu ordinal
    for e in events(root, "ExecutorThread"):
        lab = e["data"].get("Spawned", {}).get("label", "")
        g = None
        if "-gpu" in lab:
            try:
                g = int(lab.split("-gpu")[1].split("-")[0])
            except ValueError:
                pass
        threads[e["id"]] = (g, lab)
    queries = {}
    for e in events(root, "Query"):
        k, v = next(iter(e["data"].items()))
        q = queries.setdefault(e["id"], {})
        if k == "Init":
            q["label"] = v.get("instance_name")
        q[k] = e["timestamp"]
    plans = {}
    for e in events(root, "Plan"):
        d = e["data"].get("Declaration")
        if d:
            plans[e["id"]] = tgt(d.get("query_id"))
    pipes = {}
    for e in events(root, "Operator"):
        d = e["data"].get("Declaration")
        if d:
            qid = plans.get(tgt(d.get("plan_id")))
            pipes[e["id"]] = (queries.get(qid, {}).get("label"), d.get("type_name"), d.get("label"))
    for qid, q in queries.items():
        if "label" in q:
            qrows.append([q["label"], q.get("Init"), q.get("Executing"), q.get("Finished"),
                          (q["Finished"] - q["Init"]) / 1e6 if "Finished" in q and "Init" in q else ""])
    for pid, (ql, tn, lab) in pipes.items():
        prow.append([pid, ql, tn, lab])
    tasks = defaultdict(list)
    for e in events(root, "Task"):
        tasks[e["id"]].append(e)
    for tid, evs in tasks.items():
        evs.sort(key=lambda e: (e["data"][next(iter(e["data"]))].get("seq", 0), e["timestamp"]))
        r = dict(task=tid, created=None, routed=None, pref=None, reserving=None, req_bytes=None,
                 preparing=None, origin=None, in_bytes=None, first_compute=None, finalize=None,
                 success=None, gpu=None, thread=None, pipeline=None)
        comps = []
        for e in evs:
            k, v = next(iter(e["data"].items()))
            ts = e["timestamp"]
            if k == "Created":
                r["created"] = ts; r["pipeline"] = tgt(v.get("pipeline_uuid"))
            elif k == "Routing":
                r["routed"] = ts; r["pref"] = v.get("preferred_device_id")
            elif k == "Reserving":
                r["reserving"] = ts; r["req_bytes"] = v.get("requested_bytes")
            elif k == "Preparing":
                r["preparing"] = ts; r["origin"] = v.get("origin_tier"); r["in_bytes"] = v.get("input_bytes")
                th = tgt(v.get("executor_thread"))
                r["thread"] = threads.get(th, (None, th))[1]; r["gpu"] = threads.get(th, (None,))[0]
            elif k == "Computing":
                comps.append((ts, v.get("current_operator_id"), v.get("input_bytes"), v.get("peak_allocated_bytes")))
                if r["first_compute"] is None:
                    r["first_compute"] = ts
            elif k == "Finalizing":
                r["finalize"] = ts; r["success"] = v.get("success")
        ql, tn, lab = pipes.get(r["pipeline"], (None, None, None))
        r["query"] = ql; r["pipeline_type"] = tn
        trows.append(r)
        for i, (ts, op, ib, peak) in enumerate(comps):
            end = comps[i + 1][0] if i + 1 < len(comps) else r["finalize"]
            orows.append([tid, ql, tn, r["gpu"], op, ts, end, ib, peak])

cols = ["query", "pipeline_type", "task", "gpu", "thread", "pref", "created", "routed", "reserving", "preparing",
        "first_compute", "finalize", "origin", "in_bytes", "req_bytes", "success", "pipeline"]
with open(os.path.join(out, "tasks.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(cols)
    for r in trows:
        w.writerow([r.get(c) for c in cols])
with open(os.path.join(out, "task_ops.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["task", "query", "pipeline_type", "gpu", "operator_id", "start", "end", "input_bytes", "peak_alloc"])
    w.writerows(orows)
with open(os.path.join(out, "queries.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["query", "init", "executing", "finished", "wall_ms"]); w.writerows(qrows)
with open(os.path.join(out, "pipelines.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["pipeline", "query", "type_name", "label"]); w.writerows(prow)

# per query x gpu summary
qstart = {q[0]: q[1] for q in qrows}
agg = defaultdict(lambda: dict(n=0, iv=[], task_ms=0.0, prep_ms=0.0, h2d=0, first=None, last=None))
for r in trows:
    if r["query"] is None or r["preparing"] is None or r["finalize"] is None:
        continue
    a = agg[(r["query"], r["gpu"])]
    a["n"] += 1
    a["iv"].append((r["preparing"], r["finalize"]))
    a["task_ms"] += (r["finalize"] - r["preparing"]) / 1e6
    if r["first_compute"]:
        a["prep_ms"] += (r["first_compute"] - r["preparing"]) / 1e6
    if r["origin"] == "HOST" and r["in_bytes"]:
        a["h2d"] += r["in_bytes"]
    a["first"] = min(a["first"] or r["preparing"], r["preparing"])
    a["last"] = max(a["last"] or r["finalize"], r["finalize"])


def union_ms(iv):
    iv.sort(); tot = 0; cs = ce = None
    for s, e in iv:
        if cs is None or s > ce:
            if cs is not None:
                tot += ce - cs
            cs, ce = s, e
        else:
            ce = max(ce, e)
    if cs is not None:
        tot += ce - cs
    return tot / 1e6


with open(os.path.join(out, "per_query_gpu.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["query", "gpu", "tasks", "busy_ms", "sum_task_ms", "sum_prepare_ms", "h2d_bytes", "first_ms", "last_ms"])
    for (q, g), a in sorted(agg.items(), key=lambda x: (str(x[0][0]), str(x[0][1]))):
        s0 = qstart.get(q) or a["first"]
        w.writerow([q, g, a["n"], round(union_ms(a["iv"]), 2), round(a["task_ms"], 2), round(a["prep_ms"], 2), a["h2d"],
                    round((a["first"] - s0) / 1e6, 2), round((a["last"] - s0) / 1e6, 2)])
print(f"{len(qrows)} queries, {len(trows)} tasks, {len(orows)} op-phases -> {out}")
