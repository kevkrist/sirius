#!/bin/bash
# DuckDB CPU (no Sirius) cardinality checks of Bloom build sides. Run under the machine lock (96-core CPU load).
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler TMPDIR=$R/tmp
cd $R/repo
pixi run python - <<'PY'
import duckdb,time
c=duckdb.connect(); c.execute("SET temp_directory='/opt/dlami/nvme/dfperf/tmp/duck_verify'"); c.execute("SET memory_limit='300GB'")
D='/opt/dlami/nvme/dfperf/data/tpch_parquet_sf1000'
for t in ['orders','part','partsupp','lineitem','customer']:
    c.execute(f"CREATE VIEW {t} AS SELECT * FROM read_parquet('{D}/{t}/*.parquet')")
Q={
 'q21 j18 (expect 730806711 rows)': "select count(*) from orders where o_orderstatus='F'",
 'q10 j10 (expect 57360614)': "select count(*) from orders where o_orderdate >= date '1994-03-01' and o_orderdate < date '1994-06-01'",
 'q4 j9 (expect 57356572)': "select count(*) from orders where o_orderdate >= date '1996-10-01' and o_orderdate < date '1997-01-01'",
 'q9 j29 (expect 43481836): rows, distinct ps_partkey, distinct ps_suppkey': "select count(*), count(distinct ps_partkey), count(distinct ps_suppkey) from partsupp where ps_partkey in (select p_partkey from part where p_name like '%yellow%')",
 'q12 j8 (expect 31165956): rows, distinct l_orderkey': "select count(*), count(distinct l_orderkey) from lineitem where l_shipmode in ('TRUCK','REG AIR') and l_commitdate < l_receiptdate and l_shipdate < l_commitdate and l_receiptdate >= date '1994-01-01' and l_receiptdate < date '1995-01-01'",
}
for k,q in Q.items():
    t=time.time(); r=c.execute(q).fetchall(); print(k,'=>',r[0],'%.1fs'%(time.time()-t),flush=True)
PY
