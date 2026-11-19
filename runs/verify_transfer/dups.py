import duckdb, time
D="/opt/dlami/nvme/dfperf/data/tpch_parquet_sf100"
con=duckdb.connect(); con.execute("SET threads=48; SET temp_directory='/opt/dlami/nvme/dfperf/tmp'")
for t in ["lineitem","orders","part","partsupp","supplier","nation","customer"]:
    con.execute(f"create view {t} as select * from read_parquet('{D}/{t}/*.parquet')")
qs={
"q21 j18 orders F (o_orderkey)": "select count(*), count(distinct o_orderkey) from orders where o_orderstatus='F'",
"q21 j33 l1 x supplier(SAUDI) x orders(F) (l_orderkey)": """select count(*), count(distinct l_orderkey) from lineitem, supplier, nation, orders
   where s_suppkey=l_suppkey and s_nationkey=n_nationkey and n_name='SAUDI ARABIA' and l_receiptdate>l_commitdate and o_orderkey=l_orderkey and o_orderstatus='F'""",
"q9 j29 partsupp x part(green) (ps_partkey, ps_suppkey)": "select count(*), count(distinct ps_partkey), count(distinct ps_suppkey) from partsupp, part where p_partkey=ps_partkey and p_name like '%green%'",
"q10 j21 lineitem(R) x orders(1993Q4) (o_custkey)": """select count(*), count(distinct o_custkey) from lineitem, orders where l_orderkey=o_orderkey and l_returnflag='R'
   and o_orderdate >= date '1993-10-01' and o_orderdate < date '1994-01-01'""",
"q12 j8 lineitem filtered (l_orderkey)": """select count(*), count(distinct l_orderkey) from lineitem where l_shipmode in ('MAIL','SHIP') and l_commitdate<l_receiptdate
   and l_shipdate<l_commitdate and l_receiptdate>=date '1994-01-01' and l_receiptdate<date '1995-01-01'""",
"q7 j35 orders x customer(FRANCE,GERMANY) (o_orderkey)": """select count(*), count(distinct o_orderkey) from orders, customer, nation where o_custkey=c_custkey and c_nationkey=n_nationkey and n_name in ('FRANCE','GERMANY')""",
}
for k,q in qs.items():
    t=time.time(); r=con.execute(q).fetchone(); print(k, r, "dup x%.2f"%(r[0]/min(r[1:])), "%.1fs"%(time.time()-t), flush=True)
