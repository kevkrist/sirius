#!/usr/bin/env python3
"""Warm interleaved late-mat ON/OFF A/B for the TPC-H queries that engage late-mat."""
import os, re, statistics, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DB, DUCK, ITERS = os.path.join(ROOT, "test_datasets/tpch_sf50.duckdb"), os.path.join(ROOT, "build/release/duckdb"), 5
QDIR = os.path.join(ROOT, "test/tpch_performance/tpch_queries/orig")
NAMES = ["q1", "q4", "q12"]
PROBES = [(n, open(os.path.join(QDIR, n + ".sql")).read().strip().rstrip(";")) for n in NAMES]

lines = ["SET gpu_execution=true;", "SET enable_gpu_duckdb_native_scan=true;", ".timer off"]
def stmt(label, sw, q):
    lines.extend([f".print ==={label}===", f"SET late_materialize_native_strings={sw};",
                  ".timer on", q + ";", ".timer off"])
for name, q in PROBES:
    stmt(f"{name}|WARM|on", "true", q); stmt(f"{name}|WARM|off", "false", q)
    for i in range(ITERS):
        stmt(f"{name}|t{i}|on", "true", q); stmt(f"{name}|t{i}|off", "false", q)
p = subprocess.run([DUCK, DB, "-batch"], input="\n".join(lines) + "\n", capture_output=True, text=True)
cur, times = None, {}
for line in p.stdout.splitlines():
    m = re.match(r"===(.+?)===", line)
    if m: cur = m.group(1); continue
    m = re.search(r"Run Time \(s\): real ([\d.]+)", line)
    if m and cur:
        name, tag, sw = cur.split("|")
        if tag != "WARM": times.setdefault((name, sw), []).append(float(m.group(1)))
        cur = None
print(f"{'query':<8}{'OFF med(s)':>12}{'ON med(s)':>12}{'speedup':>10}{'OFF min':>10}{'ON min':>10}")
for name in NAMES:
    off, on = times.get((name, "off"), []), times.get((name, "on"), [])
    if off and on:
        mo, mn = statistics.median(off), statistics.median(on)
        print(f"{name:<8}{mo:>12.4f}{mn:>12.4f}{mo/mn:>9.2f}x{min(off):>10.4f}{min(on):>10.4f}")
