#!/usr/bin/env python3
"""compression_lever.py: per-query wire-byte savings from (a) o_comment snappy 2.6x, (b) l_shipmode dictionary,
(c) enabling the *_disabled.txt plans for part/partsupp/customer/supplier; priced on g1 and g4 wall.

Byte model for raw tables: raw width per column (SF1000 avg) / plan ratio, applied as a projection-level ratio to the
MEASURED wire bytes of that (query, table) from g4_table_bytes.csv. Wall pricing: dT = dB/B_q * copy_q * k * s, where
copy_q = busier-switch scan-copy union (nsys, profiled), k = unprofiled/profiled suite ratio, s in {0.63, 1.0}
(0.63 = measured wall sensitivity to link time from g2s->g2x; 1.0 = fully link-bound).
"""
import csv, sys, collections
sys.path.insert(0, "/home/ubuntu/sirius/test/tpch_performance")
from tpch_pin_columns import QUERY_COLUMNS  # noqa: E402

# raw bytes/row (fixed width, or avg chars + 4B offset) and plan ratio (plans/tpch_sf1000/*_disabled.txt)
COL = {
    "p_partkey": (4, 254.4), "p_name": (36.6, 2.67), "p_mfgr": (18, 46.9), "p_brand": (12, 18.9), "p_type": (24.6, 24.4),
    "p_size": (4, 5.27), "p_container": (11.6, 2.89), "p_retailprice": (8, 3.74), "p_comment": (18, 2.03),
    "ps_partkey": (4, 101.1), "ps_suppkey": (4, 37.4), "ps_availqty": (4, 2.27), "ps_supplycost": (8, 3.74), "ps_comment": (128, 1.0),
    "c_custkey": (4, 193.8), "c_name": (22, 4.37), "c_address": (29, 1.44), "c_nationkey": (4, 6.31), "c_phone": (19, 2.32),
    "c_acctbal": (8, 3.03), "c_mktsegment": (12.7, 33.9), "c_comment": (76.5, 2.77),
    "s_suppkey": (4, 170.6), "s_name": (22, 3.0), "s_address": (29, 1.43), "s_nationkey": (4, 6.31), "s_phone": (19, 2.5),
    "s_acctbal": (8, 3.03), "s_comment": (66, 2.5),
}
RAW_TABLES = {"part", "partsupp", "customer", "supplier"}

D = "/home/ubuntu/sirius-scaling-20261105/10-analysis/X3/"
tb = collections.defaultdict(float)
for r in csv.DictReader(open(D + "g4_table_bytes.csv")):
    tb[(int(r["query"]), r["table"])] += float(r["wire_GB"])


def copy_ms(f):
    out = {}
    for line in open(f):
        p = line.split()
        if p and p[0].isdigit(): out[int(p[0])] = float(p[5])
    return out


c4 = copy_ms(D + "g4_interior_scan2.txt"); c1 = copy_ms(D + "g1_interior_scan2.txt")
K4, K1 = 20.04 / 21.777, 35.82 / 36.863
B4 = {int(r["query"]): (float(r["h2d"]) - float(r.get("h2d_staging") or 0)) / 1e9 for r in csv.DictReader(open(D + "g4_link.csv"))}
B1 = {int(r["query"]): float(r["h2d"]) / 1e9 for r in csv.DictReader(open(D + "g1_link.csv"))}
rows = []; tot = collections.Counter()
for q in range(1, 23):
    sv = collections.Counter()
    for t in RAW_TABLES:
        cols = QUERY_COLUMNS[q].get(t, [])
        if not cols or tb[(q, t)] == 0: continue
        raw = sum(COL[c][0] for c in cols); comp = sum(COL[c][0] / COL[c][1] for c in cols)
        sv["raw_tables"] += tb[(q, t)] * (1 - comp / raw)
    if "o_comment" in QUERY_COLUMNS[q].get("orders", []):
        sv["o_comment"] += (tb[(q, "orders")] - 1.5e9 * 4 / 1.14 / 1e9) * (1 - 1 / 2.6)
    if "l_shipmode" in QUERY_COLUMNS[q].get("lineitem", []):
        sv["l_shipmode"] += 6e9 * 8.29 / 1.7 / 1e9 * (1 - 1.7 / 20)  # str_split 1.7x -> dictionary ~20x
    dB = sum(sv.values())
    r = dict(query=q, **{k: round(v, 2) for k, v in sv.items()}, dB=round(dB, 2), B4=round(B4[q], 1))
    for s in (0.63, 1.0):
        d4 = dB / B4[q] * c4[q] / 1e3 * K4 * s; d1 = dB / B1[q] * c1[q] / 1e3 * K1 * s
        r[f"dT4_s{s}"] = round(d4, 3); r[f"dT1_s{s}"] = round(d1, 3); r[f"dgap_s{s}"] = round(d4 - d1 / 4, 3)
    rows.append(r)
    for k, v in r.items():
        if k != "query": tot[k] += v
keys = ["query", "raw_tables", "o_comment", "l_shipmode", "dB", "B4", "dT4_s0.63", "dT4_s1.0", "dT1_s0.63", "dT1_s1.0", "dgap_s0.63", "dgap_s1.0"]
w = csv.DictWriter(sys.stdout, keys, extrasaction="ignore"); w.writeheader()
for r in rows: w.writerow(r)
w.writerow({k: round(tot[k], 2) for k in keys[1:]} | {"query": "TOT"})
