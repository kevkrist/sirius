# Date DIPs — Research & Design Notes

> Working notes for the "date DIP" optimization (branch `kk/date-dips-phase1`).
> Captures the mechanism, the literature lineage, the TPC-H ad-hoc-compliance
> analysis, the GPU/host-pin interaction, and the open design decisions.
> Last updated: 2026-06-30.

## 1. What this is

**Date DIPs = cross-table soft date-correlation pruning.** When a query filters a
**dimension** date (`orders.o_orderdate`) and FK-joins an **un-filtered fact** table
(`lineitem`) that is physically **clustered** on a *correlated* fact date
(`l_shipdate`), we translate the dimension filter into a **prune-only range predicate**
on the fact date and inject it onto the fact scan. DuckDB's native row-group zonemap
pruner then skips fact row groups before decode.

The enabling fact is a cross-column **lag** (`l_shipdate − o_orderdate ∈ [lag_lo, lag_hi]`
days) that single-column statistics cannot express — so today it is **measured** and
**cached in memory**.

This is the SQL Server `DATE_CORRELATION_OPTIMIZATION` family (a *soft* correlation),
**distinct** from the Orr et al. join-*key* DIP (which needs no measured correlation but
gives ~0 pruning here — see §5).

## 2. Status (branch `kk/date-dips-phase1`, 2026-06-30)

Implemented and validated (builds clean; 11 `[date_dip]` unit tests pass; e2e on a
`l_shipdate`-clustered SF50 dataset: Q5/Q8/Q10 ≈ 1.9–2.1× end-to-end, results
byte-identical on/off):

- **Pure core** (`sirius_date_dip.{hpp,cpp}`): `extract_date_window`,
  `derive_forward_window` (lag + lag-histogram), `make_prune_only_date_filter`.
- **The pass**: `apply_date_dips` — plan-walk that matches a date-filtered dimension GET
  to an FK-joined, date-unfiltered fact GET (INNER/SEMI only) and injects a top-level
  `OptionalFilter`.
- **Cache**: in-memory, session-lifetime `date_correlation_cache_` on `SiriusContext`
  (`upsert_date_correlation` / `all_date_correlations`). **Not persisted to disk.**
- **Measurement**: `CALL sirius_measure_date_correlation(dim_table, dim_date_col,
  dim_key_col, fact_table, fact_date_col, fact_key_col [, bucket_days])` — runs
  `min/max(fact_date − dim_date)` over the FK join under an `InternalQueryGuard` + fresh
  `Connection`; optional `bucket_days` produces a per-dim-date lag histogram.
- **Setting**: `enable_date_dips` (default **off**).
- **Staleness gate** (added 2026-06-30): a write-activity token
  `(SingleFileBlockManager::GetCheckpointIteration(), StorageManager::GetWALSize())`
  stamped at measurement and re-checked in the pass; the DIP fires only on an exact match
  (positive-confirmation, fail-closed). See §6.

**Not yet done:** the work is a Phase-1 prototype. The biggest open item is **replacing
the manual `CALL` with automatic discovery** (see §7–§8). All of it was uncommitted until
this checkpoint.

## 3. Files

| File | Role |
|---|---|
| `src/include/transparent/date_correlation.hpp` | dependency-free value type (`date_correlation`, `lag_bucket`) + the staleness `snapshot_*` fields |
| `src/include/transparent/sirius_date_dip.hpp` | pure-core API + `apply_date_dips` + `db_write_token` / `read_db_write_token` |
| `src/transparent/sirius_date_dip.cpp` | pure core, the plan-walk pass, the staleness gate, the token reader |
| `src/transparent/sirius_optimizer_extension.cpp` | `maybe_apply_date_dips` hook (fires the pass on the live plan pre-Copy) |
| `src/sirius_extension.cpp` | the `CALL` table function + `enable_date_dips` setting + measure-time token stamp |
| `src/sirius_context.{hpp,cpp}` | the in-memory correlation cache |
| `test/cpp/transparent/test_date_dip.cpp` | unit tests for the pure core |
| `test/tpch_performance/measure_date_dip_pruning.sql` | offline pruning-ceiling measurement |
| `CMakeLists.txt` | build glue (+ `-fno-gnu-unique` on `sirius_extension.cpp` to fix a mold/STB_GNU_UNIQUE link clash) |

## 4. Mechanism & soundness

- **Prune-only via `OptionalFilter`.** The derived range is wrapped in a top-level
  `duckdb::OptionalFilter`, which is (a) honored by the row-group pruner via
  `CheckStatistics`, yet (b) skipped row-wise by `convert_table_filters_to_expression`
  (`src/op/scan/scan_utils.cpp:68`). So it can only **prune row groups, never drop a
  decoded row**. The real `o_orderdate` predicate still runs post-join.
- **Necessary condition, false-positives only.** The derived window is an
  *over-approximation*: given a sound lag, every joining fact row satisfies it. Worst case
  is decoding a row group we could have skipped — never a wrong answer. (Same soundness
  contract as Orr's "necessary condition σ⇒d" and Pando's "complete but not precise.")
- **Lag histogram** (`bucket_days > 0`): per-dim-date-bucket `[lo,hi]`; tighter pruning when
  the lag drifts over time (seasonality). Collapses to the global lag when stationary
  (TPC-H). The injection is always a single interval.
- **Gating** (`try_inject_fact`): INNER/SEMI join only (OUTER/ANTI could keep unmatched
  fact rows); genuine FK link on the named key columns; fact date column **unfiltered** (so
  the `OptionalFilter` stays top-level → row-level no-op); base tables only.

## 5. Literature lineage (read these before changing the design)

- **Orr, Kandula, Chaudhuri — "Pushing Data-Induced Predicates Through Joins" (VLDB 2020).**
  The DIP of record: a join-**key** predicate derived at QO time from single-column
  zone-maps via column equivalence; *recomputed per query, nothing cached*, always sound.
  **Gives ~0 pruning for the TPC-H date case** because `o_orderdate ⊥ o_orderkey` (a date
  filter leaves surviving orderkeys spanning the whole keyspace → C2 fails; `l_shipdate`-
  clustered `lineitem` has non-selective `l_orderkey` zone-maps → C3 fails). §4.1 "Coping
  with data updates" = **taint bits** + **grow-only range-sets**. This is the *compliant,
  stateless* mechanism — but not the one that helps our dates.
- **Kimura, Huo, Rasin, Madden, Zdonik — "Correlation Maps" (VLDB 2009).** The canonical
  generalization of our soft-correlation. A CM is a **single-table** map `u → S_c`
  (unclustered value → set of clustered values), queried as **superset + re-apply original
  predicate**. Key takeaways: our single lag `[lo,hi]` = the *coarsest* CM; our lag
  histogram = a *bucketed* CM; CMs are sound **because synchronously maintained** (a stale
  CM that missed an insert ⇒ false negative ⇒ wrong answer) — independent validation that
  the staleness gate is mandatory. Strength statistic `c_per_u`. Explicitly critiques SQL
  Server's datetime correlation (= us) for being datetime-only / fixed month buckets / no
  cost model.
- **Sudhir, Tao, Laptev, Habis, Cafarella, Madden — "Pando" (VLDB 2023).** Uses **our exact
  TPC-H scenario** verbatim. Automatic, workload-driven layout optimizer: decouples k
  logical "partitioning trees" (incl. **join-induced cuts**) from physical blocking;
  discovers correlations implicitly via a data-driven cost model — **no human declaration,
  no column naming**. Caveats: needs a representative workload and **physically reorganizes
  data** (out of scope for us). It benchmarks the **pure-runtime, no-reorg analog — diPs —
  which is what Sirius actually is.** Maintenance: restrict to **FK→PK / dimension→fact**;
  under referential integrity, inserts/deletes don't break the logical→physical mapping.
- **SQL Server `DATE_CORRELATION_OPTIMIZATION`** — the cross-table datetime special case we
  descend from (a maintained MV of co-occurring datetime values).

## 6. Staleness / maintenance

- **One-directional asymmetry** (confirmed by both Orr §4.1 and CM): an over-**wide** lag is
  always **safe** (over-prune-approximation ⇒ less pruning); an under-**narrow** lag (true
  lag exceeds cached `[lo,hi]`) is the **only** wrong-answer direction. Deletes only shrink
  the true lag ⇒ safe. Inserts/updates can widen it ⇒ dangerous.
- **The write-activity token** (`db_write_token`): `(checkpoint_iteration, wal_size)` per
  `AttachedDatabase`. Read-safe (the WAL is gated by `ChangesMade()`, so reads never move
  it — unlike `last_commit`, which advances on every commit including SELECTs and is
  therefore useless here). Durable + monotone (`iteration` persists in the DB header).
  Token unchanged ⇒ no committed write since measurement. Catches insert/update/delete
  (watches write *activity*, not data-state delta). O(1), public accessors, no submodule
  patch. **Fail-closed**: the DIP fires only on a positive token match; in-memory DBs (no
  `SingleFileBlockManager`) / unverifiable / mismatch all skip.
- **Simplification (per Kevin, 2026-06-30):** in the target setting there are **no
  updates, only inserts/deletes, handled separately** — so the cached pruning metadata
  stays valid and the heavy staleness machinery is largely unnecessary; re-derive when the
  separate insert/delete path fires.
- **Rejected staleness signals** (don't re-derive): catalog version (DDL-only); row count
  (invariant under in-place UPDATE); MVCC insert/update stamps (ephemeral, GC'd off our
  schedule); global `last_commit` (moves on reads → self-invalidates).

## 7. TPC-H ad-hoc compliance (the critical constraint)

Verified against the **official TPC-H spec v3.0.1** (clause numbers below).

- **The manual `CALL` is non-compliant** on three grounds:
  - **5.2.7** — config "shall not make reference to specific tables, indices or queries for
    the purpose of providing hints to the query optimizer," nor "take special advantage of
    the limited functions actually exercised by the benchmark." Naming `o_orderdate`/
    `l_shipdate` is exactly that.
  - **5.2.8** — statistics gathering may not use "knowledge of the cardinality, values or
    distribution of a non-key column," and "must rely solely on schema-related attributes
    of a column and ... applied consistently across all tables."
  - **0.2(d)** — no "special advantage of the limited nature of TPC benchmarks ... that
    would not be generally applicable."
- **Corrections to earlier assumptions:**
  - The ad-hoc/config rules are **5.2.7–5.2.8 + 0.2**, *not* Clause 1.5.
  - The spec **permits manual DBA configuration** (5.2.7) based on general workload + schema/
    layout knowledge. So "must be automatic" is **not** the requirement — **"not
    query-specific, generally applicable"** is.
- **The deeper bar — Clause 1.5.7:** auxiliary data structures may "reference no more than
  one base table," and cross-table column materialization is barred. So a **persisted
  cross-table correlation** is non-compliant *regardless of how it's discovered*.
  **Single-column** date structures are explicitly allowed.
- **Why we may be closer than it looks:** the cache is **in-memory / session-transient,
  never persisted** ⇒ arguably not a 1.5.7 "auxiliary data structure" at all. The residual
  gray area is 5.2.8's "schema-related attributes of *a column*" applied to a cross-*column*
  statistic — best mitigated by generic, uniform, load-phase measurement.

## 8. GPU/host-pin interaction (from code investigation)

- date-DIP pruning and GPU/host pinning are **disjoint today**; no conflict, no
  double-count, **no correctness risk**.
- **Pinning is parquet-only** and short-circuits in `gpu_ingestible_factory::produce`
  (`gpu_ingestible_factory.cpp:41-51`) *before* any pruning ingestible is built; a pinned
  scan serves **all** batches.
- date-DIP injects **only onto base-table GETs** (`GetTable()` non-null) → never on
  `read_parquet` GETs. The `OptionalFilter` prunes **only** via the DuckDB-native path
  (`CheckStatistics` → `mark_row_groups_pruned_by_filter_stats`,
  `duckdb_native_metadata.cpp:426`).
- **The parquet path drops `OptionalFilter`s** (`convert_table_filters_to_expression`,
  `scan_utils.cpp:68`) → **the date-DIP currently does nothing on parquet.** To enable it
  there: stop dropping top-level `OptionalFilter`s / feed the raw bounds to cudf's
  `filter_row_groups_with_stats`.
- A **fully-pinned** fact table cannot be pruned (whole-table cache); add a guard to skip
  DIP injection for non-partial pinned tables if pinning is ever extended to native tables.

## 9. Path forward / open design decisions

A *cheap, pure-runtime* cross-table derivation **does not exist** for the TPC-H date case:
the lag is cross-table information absent from single-column stats; the join-key diP fails
(`orderkey ⊥ orderdate`); a post-join observation is too late to prune the fact scan; a
sample is unsound. So a **one-time sound observation is unavoidable** — the only lever is
*where it lives*. Given "cluster on the date column, no persistent cross-table structure,"
the recommended shape:

1. **Within-table date correlations (single-table CM) — the cleanest, fully-compliant
   win.** A date-clustered fact carries other correlated dates in the same row
   (`l_receiptdate`/`l_commitdate` ↔ `l_shipdate`). Translating a filter on a non-clustered
   date to the clustered-date range is **single-table** (1.5.7-legal, like a composite
   index). Helps e.g. **Q12**. Build at load as single-table stats.
2. **Cross-table lag — transient, in-memory, auto-discovered, load-phase.** Keep the
   in-memory (non-persisted) cache; **replace the manual `CALL` with generic discovery**:
   walk the FK graph, find INNER/SEMI FK→PK joins linking a date-filterable dimension date
   to a date-clustered fact date, measure the lag for **all** such pairs uniformly during
   the load/`ANALYZE` phase (untimed). Prune-only, FK→PK only, re-derive on insert/delete.
   Defensible (transient + generic + uniform); gray only on 5.2.8 cross-column.
3. **Join-key diP — free, compliant, ~0 on TPC-H dates.** Keep as general insurance and to
   strengthen the "general optimizer technology, not a benchmark special case" story.

**Single highest-leverage change:** delete `sirius_measure_date_correlation('orders', …)`
and replace it with a generic plan-/load-time auto-discovery pass (FK constraints +
clustering/sort metadata + the dimension filter already in the plan). That one move turns
the feature from a hack into legitimate, transient, query-agnostic optimizer machinery.

**Other open items:** decide the scan-path target (native `.db` works today vs parquet/
pinned needs the `OptionalFilter`-passthrough work); a `c_per_u`-style strength gate/
diagnostic (softened because a useless prune-only DIP is a no-op, not a regression);
sharing one code path between the within-table and cross-table cases.

## 10. Related memory

- `date-dips-feasibility` — the original feasibility eval, mechanism verdicts, measured
  pruning ceilings, performance numbers.
- `date-dip-staleness-detection` — the write-token design + the Orr §4.1 / CM maintenance
  cross-reference.
- `native-zonemap-pruning` — the Phase-0 row-group pruning this builds on.
