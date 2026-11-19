#!/usr/bin/env python3
"""edge.py <tree> <out dir> : edge-path smoke for the OP1A continuation (verifier). Run via edge.sh under the lock.
One process, q3's pinned tables (host pin + pin compression, as the timing runs):
  1. q3 x3 (warm, 2 hot)      -> reference result + hot time
  2. q3 x20 back to back      -> every result identical to the reference (rare-race check)
  3. interrupt sweep          -> con.interrupt() from a second thread at 24 delays across the query, each followed by nothing;
                                 then q3 x2 again: result identical, time not degraded
  4. empty build join, tiny build join, LIMIT join -> complete, counts printed
"""
import os, sys, time, threading

tree, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
os.environ["SIRIUS_LOG_DIR"] = out
os.environ["SIRIUS_PIN_TIER"] = "host"
os.chdir(tree)
sys.path.insert(0, os.path.join(tree, "test/tpch_performance"))
import performance_test as pt  # noqa: E402
from queries import queries_for_scale_factor  # noqa: E402
from tpch_pin_columns import emit_pin  # noqa: E402

DATA = "/opt/dlami/nvme/dfperf/data/tpch_parquet_sf1000"
pt.PIN_COMPRESSION_PLAN_DIR = "/home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000"


def say(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


con = pt.open_connection(DATA, gpu_execution=True, data_source="parquet")
con.execute("SET gpu_execution = true;")
EQ = int(os.environ.get("EDGE_Q", "3"))
pt._execute_multi(con, emit_pin(EQ, DATA, "parquet", None))
q3 = queries_for_scale_factor(1000)["q%d" % EQ]


def run(sql):
    t = time.perf_counter()
    r = con.execute(sql)
    e = time.perf_counter() - t
    return e, r.fetchall()


ref = None
times = []
for i in range(3):
    e, rows = run(q3)
    times.append(e)
    ref = rows
say("PHASE1 q3 x3 ms", [round(x * 1000, 1) for x in times], "rows", len(ref))
hot = min(times[1:])

bad = 0
ts = []
for i in range(20):
    e, rows = run(q3)
    ts.append(e)
    if rows != ref:
        bad += 1
say("PHASE2 q3 x20: mismatching results", bad, "median ms", round(sorted(ts)[10] * 1000, 1), "max ms", round(max(ts) * 1000, 1))

outcomes = []
for k in range(24):
    delay = 0.05 + k * (hot - 0.05) / 23.0
    th = threading.Timer(delay, con.interrupt)
    t = time.perf_counter()
    th.start()
    try:
        r = con.execute(q3)
        rows = r.fetchall()
        res = "completed" + ("" if rows == ref else " RESULT-DIFF")
    except Exception as ex:  # noqa: BLE001
        res = type(ex).__name__ + ": " + str(ex)[:60].replace("\n", " ")
    e = time.perf_counter() - t
    th.cancel()
    th.join()
    outcomes.append((round(delay * 1000), round(e * 1000), res))
    time.sleep(0.05)
for o in outcomes:
    say("PHASE3 interrupt at ms", o[0], "returned after ms", o[1], o[2])
post = []
okres = True
for i in range(3):
    e, rows = run(q3)
    post.append(e)
    okres = okres and rows == ref
say("PHASE3 post-interrupt q3 x3 ms", [round(x * 1000, 1) for x in post], "results identical", okres)

edge = {
    "empty_build": "select count(*) from lineitem, orders where l_orderkey = o_orderkey and o_orderdate > date '2100-01-01'",
    "one_day_build": "select count(*), sum(l_extendedprice) from lineitem, orders where l_orderkey = o_orderkey and o_orderdate = date '1995-03-15'",
    "limit_join": "select count(*) from (select l_orderkey from lineitem, orders where l_orderkey = o_orderkey and o_orderdate < date '1995-03-15' limit 5) t",
    "semi_join": "select count(*) from orders where o_orderdate < date '1993-01-01' and exists (select 1 from lineitem where l_orderkey = o_orderkey and l_shipdate > date '1993-06-01')",
}
for name, sql in (edge.items() if EQ == 3 else []):
    for i in range(2):
        try:
            e, rows = run(sql)
            say("PHASE4", name, "iter", i, "ms", round(e * 1000, 1), "rows", rows)
        except Exception as ex:  # noqa: BLE001
            say("PHASE4", name, "iter", i, "EXC", type(ex).__name__, str(ex)[:200].replace("\n", " "))
e, rows = run(q3)
say("FINAL q3 ms", round(e * 1000, 1), "result identical", rows == ref)
with open(os.path.join(out, "q3_ref.txt"), "w") as f:
    for r in ref:
        f.write(repr(r) + "\n")
say("DONE")
