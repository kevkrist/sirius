#!/usr/bin/env python
"""Verifier: hang / correctness probe of non-TPC-H join shapes (unpinned parquet scans, SF1000).
Usage (cwd = repo whose extension is loaded): shapes.py <out.jsonl> <timeout_s> [cpu] [only=<name,...>]
Each shape runs twice. A watchdog kills the process (exit 3, HANG record) when one statement exceeds the timeout.
An 'interrupt' pseudo-shape cancels a running query after 0.25 s and then reruns a reference query."""
import sys, os, json, time, hashlib, threading
REPO = os.getcwd()
sys.path.insert(0, os.path.join(REPO, "test/tpch_performance"))
import duckdb
import performance_test as pt

out_path, timeout_s = sys.argv[1], float(sys.argv[2])
cpu = "cpu" in sys.argv[3:]
only = None
for a in sys.argv[3:]:
    if a.startswith("only="): only = set(a[5:].split(","))

SHAPES = [
 ("nofilter_plan", "select count(*), sum(o_totalprice) from orders join customer on o_custkey = c_custkey"),
 ("declined_domain", "select count(*), sum(o_totalprice) from orders join customer on o_custkey = c_custkey where c_acctbal > -990"),
 ("selective", "select count(*), sum(o_totalprice) from orders join customer on o_custkey = c_custkey where c_mktsegment = 'BUILDING'"),
 ("empty_build", "select count(*), sum(o_totalprice) from orders join customer on o_custkey = c_custkey where c_acctbal > 1e9"),
 ("empty_build_left", "select count(*), count(c_custkey) from supplier left join customer on s_suppkey = c_custkey and c_acctbal > 1e9"),
 ("one_row_build", "select count(*), sum(o_totalprice) from orders join customer on o_custkey = c_custkey where c_custkey = 77777"),
 ("empty_probe", "select count(*) from orders join customer on o_custkey = c_custkey where c_mktsegment = 'BUILDING' and o_totalprice < 0"),
 ("multi_filter_scan", "select count(*), sum(ps_availqty) from partsupp, part, supplier where ps_partkey = p_partkey and ps_suppkey = s_suppkey and p_size = 15 and s_nationkey = 7"),
 ("multi_filter_3", "select count(*), sum(ps_availqty) from partsupp, part, supplier, nation where ps_partkey = p_partkey and ps_suppkey = s_suppkey and s_nationkey = n_nationkey and p_size = 15 and p_brand = 'Brand#23' and n_name = 'FRANCE'"),
 ("stacked4", "select count(*), sum(o_totalprice) from orders, customer, nation, region where o_custkey = c_custkey and c_nationkey = n_nationkey and n_regionkey = r_regionkey and r_name = 'ASIA' and o_orderdate < date '1993-01-01'"),
 ("self_join", "select count(*) from customer c1 join customer c2 on c1.c_custkey = c2.c_custkey where c2.c_acctbal < -900"),
 ("self_join_orders", "select count(*), sum(o1.o_totalprice) from orders o1 join orders o2 on o1.o_orderkey = o2.o_orderkey where o2.o_totalprice > 500000"),
 ("limit10", "select count(*) from (select o_orderkey from orders join customer on o_custkey = c_custkey where c_mktsegment = 'BUILDING' limit 10) t"),
 ("limit0", "select count(*) from (select o_orderkey from orders join customer on o_custkey = c_custkey where c_mktsegment = 'BUILDING' limit 0) t"),
 ("limit_plain", "select o_orderkey is not null from orders join customer on o_custkey = c_custkey where c_mktsegment = 'BUILDING' limit 5"),
 ("limit_multi", "select count(*) from (select ps_partkey from partsupp, part, supplier where ps_partkey = p_partkey and ps_suppkey = s_suppkey and p_size = 15 and s_nationkey = 7 limit 100) t"),
 ("semi_in", "select count(*), sum(o_totalprice) from orders where o_custkey in (select c_custkey from customer where c_acctbal < -900)"),
 ("anti_notexists", "select count(*) from supplier where not exists (select 1 from customer where c_custkey = s_suppkey and c_acctbal > 0)"),
 ("mark_or", "select count(*) from supplier where s_acctbal < 0 or s_suppkey in (select c_custkey from customer where c_acctbal < -900)"),
 ("left_join", "select count(*), count(c_custkey) from orders left join customer on o_custkey = c_custkey and c_mktsegment = 'BUILDING' where o_orderdate < date '1993-01-01'"),
 ("right_join", "select count(*), count(o_orderkey) from orders right join customer on o_custkey = c_custkey and o_totalprice > 500000 where c_mktsegment = 'BUILDING'"),
 ("full_join", "select count(*), count(s_suppkey), count(c_custkey) from supplier full join customer on s_suppkey = c_custkey and c_acctbal < 0"),
 ("target_in_lower_build", "select count(*), sum(o.o_totalprice) from orders o, customer c, (select distinct ps_suppkey k from partsupp where ps_supplycost < 1.5) x where o.o_custkey = c.c_custkey and c.c_nationkey + 0 * c.c_custkey = c.c_nationkey and c.c_custkey = x.k and o.o_orderdate < date '1993-06-01'"),
 ("union_probe", "select count(*) from (select o_custkey k from orders where o_orderdate < date '1992-06-01' union all select o_custkey from orders where o_orderdate > date '1998-06-01') u join customer on k = c_custkey where c_mktsegment = 'BUILDING'"),
 ("cte_shared", "with b as materialized (select c_custkey from customer where c_mktsegment = 'BUILDING' and c_acctbal < 0) select (select count(*) from orders join b on o_custkey = c_custkey where o_orderdate < date '1993-01-01'), (select count(*) from supplier join b on s_suppkey = c_custkey)"),
 ("bushy", "select count(*) from (select o_custkey, o_orderkey from orders join customer on o_custkey = c_custkey where c_mktsegment = 'BUILDING' and o_orderdate < date '1992-04-01') a join (select ps_suppkey from partsupp join supplier on ps_suppkey = s_suppkey where s_nationkey = 7 and ps_availqty < 20) b on a.o_custkey = b.ps_suppkey"),
 ("ineq_residual", "select count(*) from orders join customer on o_custkey = c_custkey and o_totalprice > c_acctbal * 50 where c_mktsegment = 'BUILDING' and o_orderdate < date '1993-01-01'"),
 ("agg_build", "select count(*) from orders join (select c_nationkey nk, max(c_custkey) mk from customer group by c_nationkey) t on o_custkey = mk"),
 ("corr_scalar_delim", "select count(*) from supplier s where s_acctbal > (select avg(c_acctbal) from customer c where c.c_nationkey = s.s_nationkey and c.c_mktsegment = 'BUILDING')"),
 ("corr_exists_ineq", "select count(*) from supplier s where exists (select 1 from customer c where c.c_custkey = s.s_suppkey and c.c_acctbal > s.s_acctbal)"),
 ("corr_agg_join", "select count(*) from orders o, customer c where o.o_custkey = c.c_custkey and c.c_mktsegment = 'BUILDING' and o.o_orderdate < date '1992-03-01' and o.o_totalprice > (select avg(o2.o_totalprice) from orders o2 where o2.o_custkey = o.o_custkey and o2.o_orderdate < date '1992-03-01')"),
 ("star4", "select count(*), sum(ps_supplycost) from partsupp, part, supplier, nation, region where ps_partkey = p_partkey and ps_suppkey = s_suppkey and s_nationkey = n_nationkey and n_regionkey = r_regionkey and p_container = 'SM BOX' and r_name = 'EUROPE' and s_acctbal > 0"),
 ("topn_join", "select o_orderkey, o_totalprice from orders join customer on o_custkey = c_custkey where c_mktsegment = 'BUILDING' and c_acctbal > 9990 order by o_totalprice desc, o_orderkey limit 20"),
 ("window_probe", "select count(*), sum(rn) from (select o_custkey, row_number() over (partition by o_custkey order by o_orderkey) rn from orders where o_orderdate < date '1992-02-01') w join customer on o_custkey = c_custkey where c_mktsegment = 'BUILDING'"),
 ("two_keys", "select count(*) from partsupp ps1 join partsupp ps2 on ps1.ps_partkey = ps2.ps_partkey and ps1.ps_suppkey = ps2.ps_suppkey where ps2.ps_availqty < 10"),
 ("null_keys_left_chain", "select count(*), count(c_custkey), count(n_nationkey) from supplier left join customer on s_suppkey = c_custkey and c_acctbal < 0 left join nation on c_nationkey = n_nationkey and n_regionkey = 1"),
 ("string_key", "select count(*) from customer c join (select c_name nm from customer where c_acctbal > 9995) t on c.c_name = t.nm"),
]
REF = ("selective", dict(SHAPES)["selective"])

state = {"name": None, "t0": None}
fout = open(out_path, "a")
def emit(rec):
    fout.write(json.dumps(rec) + "\n"); fout.flush()
def watchdog():
    while True:
        time.sleep(1.0)
        t0, name = state["t0"], state["name"]
        if t0 is not None and time.time() - t0 > timeout_s:
            emit({"shape": name, "status": "HANG", "after_s": time.time() - t0})
            os._exit(3)
threading.Thread(target=watchdog, daemon=True).start()

con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
if not cpu:
    con.execute(f"LOAD '{pt.EXTENSION_PATH}'")
for stmt in pt._build_views_sql(os.environ["DATA"], None).split(";\n"):
    if stmt.strip(): con.execute(stmt)
con.execute("SET gpu_execution = %s" % ("false" if cpu else "true"))

def run(name, sql, it):
    state["name"], state["t0"] = name, time.time()
    rec = {"shape": name, "iter": it}
    try:
        t = time.perf_counter(); rows = con.execute(sql).fetchall(); rec["s"] = round(time.perf_counter() - t, 3)
        rec["status"] = "ok"; rec["rows"] = len(rows)
        rec["md5"] = hashlib.md5(repr(sorted(map(repr, rows))).encode()).hexdigest()
        rec["head"] = repr(rows[:2])[:200]
    except Exception as e:
        rec["status"] = "error"; rec["err"] = str(e)[:300]
    state["t0"] = None
    emit(rec); print(rec, flush=True)

for name, sql in SHAPES:
    if only and name not in only: continue
    for it in range(1 if cpu else 2):
        run(name, sql, it)

if not cpu and "nointerrupt" not in sys.argv and (not only or "interrupt" in only):
    for delay in (0.03, 0.08, 0.15, 0.25, 0.4):
        for nm in ("selective", "nofilter_plan", "stacked4"):
            sql = dict(SHAPES)[nm]
            state["name"], state["t0"] = f"interrupt_{nm}_{delay}", time.time()
            timer = threading.Timer(delay, con.interrupt); timer.start()
            rec = {"shape": state["name"]}
            try:
                con.execute(sql).fetchall(); rec["status"] = "finished_before_interrupt"
            except Exception as e:
                rec["status"] = "interrupted"; rec["err"] = str(e)[:120]
            timer.cancel(); state["t0"] = None; emit(rec); print(rec, flush=True)
            run("after_interrupt_" + nm, sql, 0)
    run("after_interrupt_ref", REF[1], 1)
state["name"], state["t0"] = "close", time.time()
con.close()
emit({"shape": "close", "status": "ok"})
