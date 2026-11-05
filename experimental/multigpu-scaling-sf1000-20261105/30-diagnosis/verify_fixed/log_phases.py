#!/usr/bin/env python3
"""Decompose per-query time outside the Quent window using sirius log timestamps (ms resolution).
Phases per TPC-H query statement (hot iters 2-4):
  plan  = QueryBegin(SQL select) -> 'physical plan generated successfully'  (DuckDB bind/optimize + Sirius optimizer hooks + Sirius plan gen)
  pre   = plan-generated -> [window] begin
  win   = [window] begin -> [window] end
  post  = [window] end -> 'Transparent GPU execution: query completed'
usage: log_phases.py <run_dir>"""
import re, sys, csv, glob
run = sys.argv[1]
log = sorted(glob.glob(f"{run}/log/sirius_*.log"))[0]
ts_re = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:(\d\d\.\d{3}))\]")
from datetime import datetime
def ts(line):
    m = ts_re.match(line)
    return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() * 1000 if m else None
label = None; cur = None; rows = []
for line in open(log, errors="replace"):
    if not line.startswith("[20"): continue
    t = ts(line)
    m = re.search(r"sirius_set_query_label\('tpch_q(\d+)_iter(\d+)'\)", line)
    if m: label = (int(m.group(1)), int(m.group(2))); continue
    if "sirius_context.cpp:334] QueryBegin" in line and label and "SQL:" in line and "sirius_set_query_label" not in line:
        cur = {"q": label, "begin": t}; continue
    if cur is None: continue
    if "physical plan generated successfully" in line and "plan" not in cur: cur["plan"] = t
    elif "[window] begin" in line and "wbeg" not in cur: cur["wbeg"] = t
    elif "[window] end" in line: cur["wend"] = t
    elif "Transparent GPU execution: query completed" in line:
        cur["done"] = t; rows.append(cur); cur = None; label = None
tim = {}
for r in csv.DictReader(open(f"{run}/timings.csv")):
    if r["query"].isdigit(): tim[(int(r["query"]), int(r["iter"]))] = float(r["seconds"]) * 1000
agg = {}
for r in rows:
    q, it = r["q"]
    if it < 2: continue
    d = dict(plan=r["plan"] - r["begin"], pre=r["wbeg"] - r["plan"], win=r["wend"] - r["wbeg"],
             post=r["done"] - r["wend"], timer=tim.get((q, it), float("nan")))
    d["unlogged"] = d["timer"] - (r["done"] - r["begin"])
    for k, v in d.items(): agg.setdefault(q, {}).setdefault(k, []).append(v)
keys = ["timer", "plan", "pre", "win", "post", "unlogged"]
print("q," + ",".join(keys))
tot = {k: 0 for k in keys}
for q in sorted(agg):
    m = {k: sum(agg[q][k]) / len(agg[q][k]) for k in keys}
    for k in keys: tot[k] += m[k]
    print(f"{q}," + ",".join(f"{m[k]:.1f}" for k in keys))
print("TOTAL," + ",".join(f"{tot[k]:.1f}" for k in keys))
