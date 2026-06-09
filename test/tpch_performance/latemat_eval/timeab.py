#!/usr/bin/env python3
"""Warm, interleaved late-mat ON/OFF wall-clock A/B for the firing probes.

All runs happen in ONE duckdb process (warm): cold runs on the GB10 are
process-init dominated, so we discard a warmup per config then interleave
ON/OFF timed runs and report medians. Each probe forces the varchar columns to
materialize via sum(length(col)) so the string decode actually executes.
"""
import os, re, statistics, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DB  = os.path.join(ROOT, "test_datasets/tpch_sf50.duckdb")
DUCK = os.path.join(ROOT, "build/release/duckdb")
ITERS = 5

# (name, force-materialize SELECT). The outer agg keeps output tiny but forces
# the full scan + string decode + filter to run on the GPU.
PROBES = [
  ("PA_fsst_0.04pct",
   "SELECT count(*), sum(length(l_comment)) "
   "FROM (SELECT l_comment FROM lineitem WHERE l_shipdate = DATE '1995-06-17')"),
  ("PD_orders_fsst2",
   "SELECT count(*), sum(length(o_comment))+sum(length(o_clerk)) "
   "FROM (SELECT o_comment, o_clerk FROM orders WHERE o_orderdate = DATE '1995-06-17')"),
  ("PE_customer_fsst3",
   "SELECT count(*), sum(length(c_name))+sum(length(c_address))+sum(length(c_comment)) "
   "FROM (SELECT c_name, c_address, c_comment FROM customer WHERE c_acctbal > 9900.00)"),
  ("PF_orders_mixed",
   "SELECT count(*), sum(length(o_orderpriority))+sum(length(o_comment)) "
   "FROM (SELECT o_orderpriority, o_comment FROM orders "
   "WHERE o_orderdate BETWEEN DATE '1996-01-01' AND DATE '1996-01-03')"),
  ("PB_lineitem_1mo",
   "SELECT count(*), sum(length(l_comment)) "
   "FROM (SELECT l_comment FROM lineitem "
   "WHERE l_shipdate BETWEEN DATE '1995-03-01' AND DATE '1995-03-31')"),
  ("Q01_dict_98pct",
   open(os.path.join(HERE, "queries/Q01_tpch.sql")).read().strip().rstrip(";")),
]

def build_script():
    lines = ["SET gpu_execution=true;",
             "SET enable_gpu_duckdb_native_scan=true;",
             ".timer off"]
    def stmt(label, sw, q):
        lines.append(f".print ==={label}===")
        lines.append(f"SET late_materialize_native_strings={sw};")
        lines.append(".timer on")
        lines.append(q + ";")
        lines.append(".timer off")
    for name, q in PROBES:
        stmt(f"{name}|WARM|on",  "true",  q)   # warmup, discarded
        stmt(f"{name}|WARM|off", "false", q)
        for i in range(ITERS):
            stmt(f"{name}|t{i}|on",  "true",  q)
            stmt(f"{name}|t{i}|off", "false", q)
    return "\n".join(lines) + "\n"

def main():
    script = build_script()
    p = subprocess.run([DUCK, DB, "-batch"], input=script,
                       capture_output=True, text=True)
    out = p.stdout
    # Parse: ===name|tag|sw=== ... Run Time (s): real N
    cur = None
    times = {}  # (name, sw) -> [reals]  (warmups excluded)
    for line in out.splitlines():
        m = re.match(r"===(.+?)===", line)
        if m:
            cur = m.group(1); continue
        m = re.search(r"Run Time \(s\): real ([\d.]+)", line)
        if m and cur:
            name, tag, sw = cur.split("|")
            if tag != "WARM":
                times.setdefault((name, sw), []).append(float(m.group(1)))
            cur = None
    if not times:
        sys.stderr.write("NO TIMINGS PARSED. stderr:\n" + p.stderr[:2000] +
                         "\n--- stdout tail ---\n" + out[-2000:])
        sys.exit(1)
    names = [n for n, _ in PROBES]
    print(f"{'probe':<20}{'OFF med(s)':>12}{'ON med(s)':>12}{'speedup':>10}"
          f"{'OFF min':>10}{'ON min':>10}")
    for name in names:
        off = times.get((name, "off"), [])
        on  = times.get((name, "on"), [])
        if not off or not on:
            print(f"{name:<20}  (missing timings)"); continue
        mo, mn = statistics.median(off), statistics.median(on)
        sp = mo / mn if mn else float('nan')
        print(f"{name:<20}{mo:>12.4f}{mn:>12.4f}{sp:>9.2f}x"
              f"{min(off):>10.4f}{min(on):>10.4f}")

if __name__ == "__main__":
    main()
