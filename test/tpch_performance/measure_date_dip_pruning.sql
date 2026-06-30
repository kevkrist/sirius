-- measure_date_dip_pruning.sql
--
-- Offline measurement of the *pruning ceiling* of a forward date DIP
-- (orders.o_orderdate filter -> derived l_shipdate window -> prune clustered
-- lineitem row groups), WITHOUT building the feature. Run against a .duckdb file
-- whose `lineitem` is clustered (sorted) on l_shipdate:
--
--   build/release/duckdb test_datasets/tpch_sf50_shipdate.duckdb < \
--     test/tpch_performance/measure_date_dip_pruning.sql
--
-- Also run it against an UNCLUSTERED file (e.g. tpch_sf50.duckdb) to see the
-- contrast (≈0% pruned — the reason clustering is a prerequisite).
--
-- Method: DuckDB stores fixed 122880-row row groups; `rowid` is the physical
-- storage position, so (rowid // 122880) reproduces the exact storage row-group
-- index (cross-checked: #distinct == pragma_storage_info row groups). The min/max
-- of l_shipdate per bucket IS the zone-map Sirius's Phase-0 pruner reads via
-- PartitionRowGroup::GetColumnStatistics. A row group is *pruned* iff its
-- [lo,hi] does not overlap the query window.
--
-- Derived SOUND window: orders.o_orderdate in [d1,d2) implies, by the dbgen
-- correlation l_shipdate = o_orderdate + rand(1..121), l_shipdate in [d1, d2+121d].
-- The "noslack" column applies the UNSOUND no-widening window [d1,d2) purely as a
-- reference ceiling, to expose how much selectivity the 121-day lag slack costs.

.mode box

CREATE TEMP TABLE rg AS
SELECT (rowid // 122880)::BIGINT AS rg_id,
       min(l_shipdate)           AS lo,
       max(l_shipdate)           AS hi,
       count(*)                  AS n
FROM lineitem
GROUP BY rg_id;

-- (0) Sanity / cross-check: bucket count should equal storage row groups.
SELECT 'sanity' AS section,
       (SELECT count(*) FROM rg)                                              AS rowid_buckets,
       (SELECT count(DISTINCT row_group_id)
          FROM pragma_storage_info('lineitem')
         WHERE column_name = 'l_shipdate')                                    AS storage_row_groups,
       (SELECT sum(n) FROM rg)                                                AS total_rows;

-- (1) Clustering quality: tighter per-row-group l_shipdate spans => higher ceiling.
SELECT 'clustering_quality' AS section,
       count(*)                                                  AS total_row_groups,
       round(median(date_diff('day', lo, hi)), 1)                AS median_rg_span_days,
       round(quantile_cont(date_diff('day', lo, hi), 0.95), 1)   AS p95_rg_span_days,
       max(date_diff('day', lo, hi))                             AS max_rg_span_days
FROM rg;

-- (2) Forward date-DIP pruning ceiling per query.
WITH q(name, d1, d2) AS (
  VALUES ('Q4',  DATE '1996-10-01', DATE '1997-01-01'),   -- o_orderdate >= '96-10-01' and < '97-01-01'
         ('Q5',  DATE '1997-01-01', DATE '1998-01-01'),   -- >= '97-01-01' and < '98-01-01'
         ('Q8',  DATE '1995-01-01', DATE '1997-01-01'),   -- between '95-01-01' and '96-12-31'
         ('Q10', DATE '1994-03-01', DATE '1994-06-01')    -- >= '94-03-01' and < '94-06-01'
),
p AS (
  SELECT name, d1, d2,
         d1                    AS ship_lo,
         d2 + INTERVAL 121 DAY AS ship_hi
  FROM q
),
tot AS (SELECT count(*) AS t FROM rg)
SELECT
  p.name,
  date_diff('day', p.d1, p.d2)                                                                   AS order_win_d,
  date_diff('day', p.ship_lo, p.ship_hi)                                                         AS ship_win_d,
  (SELECT t FROM tot)                                                                            AS total_rg,
  count(*) FILTER (WHERE NOT (rg.hi >= p.ship_lo AND rg.lo <= p.ship_hi))                         AS pruned_rg,
  round(100.0 * count(*) FILTER (WHERE NOT (rg.hi >= p.ship_lo AND rg.lo <= p.ship_hi))
        / (SELECT t FROM tot), 1)                                                                 AS pruned_pct,
  round(100.0 * sum(rg.n) FILTER (WHERE NOT (rg.hi >= p.ship_lo AND rg.lo <= p.ship_hi))
        / sum(rg.n), 1)                                                                           AS decode_saved_pct,
  round(100.0 * count(*) FILTER (WHERE NOT (rg.hi >= p.d1 AND rg.lo <= p.d2))
        / (SELECT t FROM tot), 1)                                                                 AS pruned_pct_noslack
FROM p CROSS JOIN rg
GROUP BY p.name, order_win_d, ship_win_d
ORDER BY p.name;

-- (3) Q3 is already half-pruned by its DIRECT l_shipdate > '1995-03-25' filter;
-- the DIP only ADDS an upper bound l_shipdate <= '1995-03-25' + 121d = '1995-07-24'
-- (from o_orderdate < '1995-03-25'). Show the incremental row groups it adds.
WITH tot AS (SELECT count(*) AS t FROM rg)
SELECT 'Q3_incremental' AS section,
       (SELECT t FROM tot)                                                                  AS total_rg,
       count(*) FILTER (WHERE rg.hi <= DATE '1995-03-25')                                    AS pruned_direct_only,
       count(*) FILTER (WHERE rg.hi <= DATE '1995-03-25' OR rg.lo > DATE '1995-07-24')       AS pruned_direct_plus_dip,
       count(*) FILTER (WHERE rg.lo > DATE '1995-07-24')                                     AS dip_added_row_groups
FROM rg;
