# Shuffle Elision via Partitioning Properties — Design + Phase 0 Measurements

> **Superseded:** This draft was reviewed and consolidated into
> [issue-1018-prepartitioned-shuffle-design.md](issue-1018-prepartitioned-shuffle-design.md).
> It remains as the original Phase 0 measurement note; use the consolidated document
> for architecture, compatibility rules, and implementation phasing.

**Status:** Superseded measurement note
**Issue:** [#1018 — Explore avoiding shuffles by having the data being pre-partitioned](https://github.com/sirius-db/sirius/issues/1018)
**Related:** [#995](https://github.com/sirius-db/sirius/issues/995) (small copies to multiple GPUs during partitioning), [#400](https://github.com/sirius-db/sirius/issues/400) (input data organization), [PR #1038](https://github.com/sirius-db/sirius/pull/1038) (partition coalescing + single materialization)

> A parallel draft exists at
> [issue-1018-prepartitioned-shuffle-design.md](issue-1018-prepartitioned-shuffle-design.md),
> centered on storage-level pre-partitioning (bucketed pinned layouts, external formats).
> This document reaches the same core mechanism independently, adds the **Phase 0
> measurements** that ground the phasing, and argues for a cheaper first phase:
> **plan-level propagation that elides the *second* shuffle inside a query**, before any
> storage-format work. See [Relationship to the parallel draft](#relationship-to-the-parallel-draft).

## Problem

A "shuffle" in Sirius is the `PARTITION` operator (`cudf::hash_partition`, murmur3) plus
its companions (`CONCAT`, `MERGE_*`). The pipeline converter
(`src/pipeline/sirius_pipeline_converter.cpp`: `split_join_sink()`,
`split_intermediate_joins()`, `split_group_aggregate_sink()`) inserts them
**unconditionally** around every hash join (both sides) and grouped aggregate. Nothing in
the plan tracks whether the input is *already* partitioned on the required keys, so data
that arrives correctly distributed is re-hashed and re-materialized anyway.

Two facts make elision cheap to add:

1. **Partitioning survives the operators between shuffles.** Batches carry a
   `partition_idx` (`partitioned_operator_data`,
   `src/include/op/sirius_physical_operator.hpp`); a hash join processing partition *p*
   emits output tagged *p*; filters and projections never move rows between batches.
   The machinery to carry partitioned data exists — only the bookkeeping about what the
   partitioning *means* is missing.
2. **Partition-count coordination already exists.** The build side decides the count
   (`determine_num_partitions()`, `src/op/sirius_physical_partition.cpp`) and the probe
   side follows via the `set_sibling_partition_op()` link. Pinning a count to match an
   upstream distribution is a small extension of that mechanism.

## Phase 0: what shuffles actually cost (measured)

TPC-H SF30, single NVIDIA GB10, 22 queries × 2 runs (warm run analyzed),
`SIRIUS_LOG_LEVEL=trace`, per-operator attribution from `task_outputs.csv`
(`tools/log_analyzer/parse_logs.py`). Two configs: defaults, and
`hash_partition_bytes: 64Mi` to force the engaged regime.

Findings:

- **At SF30 / single GPU / defaults, shuffles almost never engage.** 192 of 194
  `determine_num_partitions()` decisions chose **N=1** — TPC-H join build sides are
  nearly always under the 512 MB target, and the build side decides for both sides.
  N=1 PARTITION is a near-free passthrough. Baseline shuffle share: PARTITION+CONCAT =
  1.5% of operator time; only Q18 (partial group-by output > target) exceeded it (21.9%
  with merges).
- **The volume at stake is large.** Per warm benchmark pass, scans emit **63 GB**, of
  which **55 GB (87%) flows through PARTITION** and 54 GB through CONCAT. Under the
  multi-GPU floor (`min_num_partitions = num_gpus`), all of it engages real hash
  partitioning and ~(G−1)/G of the bytes cross GPUs (peer DMA or host-staging). The
  single-GPU numbers below exclude transfer cost entirely — they are a lower bound.
- **Engaged regime (64 MB target):** PARTITION+CONCAT = **11.2% of non-scan operator
  time** (3.6% of total including scans); with MERGE_* operators, **19.5% of non-scan
  time**. Per query: Q9 13.3% (P+C), Q18 32.0% (P+C+M), Q7 8.0%, Q10 8.0%, Q21 4.6%.
- **Q18 is the canonical elision target**: partial group-by on `l_orderkey` immediately
  after a join partitioned on `l_orderkey` — 128 ms PARTITION + 483 ms MERGE_GROUP_BY of
  1911 ms operator time, re-partitioning data on a key it is already partitioned by.
  Q3/Q4/Q13 share the shape; their intermediates simply stayed under the threshold at
  SF30.

Conclusion: on a single GPU at default settings the win is small; the value concentrates
in (a) **multi-GPU** (forced engagement + transfer avoidance — not measurable on the
1-GPU dev box) and (b) **memory-pressured / larger-SF** regimes (measured 11–20% of
non-scan work). This drives the phasing below.

## Design

One concept powers everything: a **partitioning property** threaded through pipeline
conversion, exploited first at plan level (skip the second shuffle in a query), then at
storage level (skip the first shuffle via pre-partitioned pinned tables).

### The `partitioning_info` descriptor

Attached to each pipeline's output during conversion:

```
partitioning_info {
  keys:            column bindings, with post-cast types
                   (gpu_partition_impl.cpp casts keys before hashing, so the type
                    is part of the identity — int32 vs int64 hashes differ)
  scheme:          HASH_MURMUR3 | NONE            (extensible)
  num_partitions:  int, or DYNAMIC (resolved at runtime by the originating PARTITION)
  origin:          the PARTITION op or pinned-table entry that established it
}
```

Propagation rules:

| Operator | Rule |
|---|---|
| PARTITION | establishes the property (keys, scheme, count) |
| CONCAT | preserves it (concatenation is within a partition) |
| HASH_JOIN | output inherits it on the join keys, remapped to output column space |
| FILTER | preserves it |
| PROJECTION | remaps key indices; **drops** the property if a key is projected away or computed |
| scan of pre-partitioned pinned table | property from `pinned_entry` metadata (Phase 2) |
| everything else | clears it |

**Compatibility:** input partitioned on keys `K_in` satisfies a consumer requiring
`K_req` iff `K_in ⊆ K_req` with matching scheme and types (equal `K_in` ⇒ same hash
bucket, so equal `K_req` rows are co-located). For joins, both sides must additionally
agree on partition count — one atomic decision for the sibling pair.

### Phase 1 — elide the second shuffle (plan-level, no storage changes)

In `split_group_aggregate_sink()` and `split_join_sink()`: before inserting
PARTITION(+CONCAT), check the input's `partitioning_info`. If compatible, skip the pair
and wire the consumer's partitioned ports directly to the upstream partitioned
repository.

- First target: **group-by after a same-key join** (Q3/Q4/Q13/Q18 shape). Ideal because
  `MERGE_GROUP_BY` accepts whatever partition count arrives — no count negotiation.
- Second target: chained joins on the same key (reuse the probe-side partitioning).
- Multi-GPU bonus: task affinity is `partition_idx % num_gpus`
  (`src/creator/task_creator.cpp`), and the index is preserved — the elided path keeps
  every partition on the GPU it is already on. Zero cross-GPU movement where today
  there is a full reshuffle.

**Memory-safety guard.** `determine_num_partitions()` sizes partitions against
`hash_partition_bytes` to bound per-task memory; an inherited count was chosen for a
*different* data size. Elide only when the inherited per-partition sizes (computed at
runtime from actual batch sizes in the repository — never from optimizer estimates)
fall within a tolerance of what `determine_num_partitions()` would choose; otherwise
fall back to shuffling exactly as today. Elision is a pure optimization with an
always-available fallback.

### Phase 2 — pre-partitioned pinned tables (storage-level)

```sql
CALL pin_table('lineitem', partition_by => ['l_orderkey']);   -- count auto or explicit
```

- Pin-time: run the same versioned hash partitioning once during ingest; store
  per-partition chunks with placement consistent with `partition_idx % num_gpus`;
  record `partitioning_info` on the `pinned_entry`
  (`src/include/scan_manager/sirius_scan_manager.hpp`).
- Scan-time: the split provider emits splits tagged with `partition_idx` and
  `preferred_device_id` = owning GPU; the converter picks the property up at plan time
  (pinned entries are visible to the scan manager then) and elides the scan-side
  PARTITION.
- Joins with only one side pre-partitioned: the other side still shuffles, but with its
  count **fixed to match** — a "fixed count" mode for the existing sibling-coordination
  mechanism, instead of "build decides". Both sides pre-partitioned on the join key ⇒
  zero shuffles.
- This is the amortization play (pin once, query many) and partially subsumes #995:
  partition-aware pinning replaces the many-small-copies partitioning phase with I/O
  placed correctly up front.

### Explicitly deferred

- **External layout trust** (Iceberg `bucket(N, col)`, hive layouts). Iceberg's bucket
  hash is not cudf's murmur3; it only helps when both sides share it or the transform is
  implemented on GPU. Per project principle, correctness is gated only on
  partitionings Sirius itself performed and recorded — never on external claims or
  optimizer statistics.
- **Partition refinement**: when an inherited count is too coarse, locally split each
  partition by re-hashing (cheap, preserves compatibility) instead of a full reshuffle.

## Risks

| Risk | Handling |
|---|---|
| Inherited count violates memory bounds | runtime size check against `determine_num_partitions()` tolerance; fall back to shuffle |
| Outer joins already incorrect with multiple partitions ([#485](https://github.com/sirius-db/sirius/issues/485)) | gate elision to partition-safe join types (start: INNER equi-join, grouped aggregate) |
| Key type-cast mismatch (int32 vs int64 hashing) | descriptor records post-cast types; compatibility check rejects mismatches |
| Stale pinned metadata | `partitioning_info` lives on the `pinned_entry`, rebuilt with it — no separate catalog to drift |
| Skew in pre-partitioned data | no worse than today's runtime hash partitioning (same hash function); skew-aware policy deferred |

## Phasing and recommendation

| Phase | Scope | Status / gate |
|---|---|---|
| 0 | Measure PARTITION/CONCAT/MERGE share on TPC-H | **Done** (this doc); redo on a multi-GPU box before Phase 2 |
| 1 | `partitioning_info` + propagation + elide group-by-after-join and chained same-key joins | justified by Q18 (32% op-time ceiling); converter-local, cheap |
| 2 | `pin_table(partition_by)` + scan-side elision + fixed-count sibling mode | gate on multi-GPU measurement (transfer avoidance is the dominant win) |
| 3 | Both-sides co-partitioned joins everywhere, partition refinement, external (Iceberg/hive) layouts | later |

## Relationship to the parallel draft

[issue-1018-prepartitioned-shuffle-design.md](issue-1018-prepartitioned-shuffle-design.md)
independently converges on the same core: a distribution property with strict semantic
compatibility (hash algorithm + seed + types + null policy + count), PARTITION kept as a
validation/fallback boundary (its `REUSE_OR_SHUFFLE`), pinned bucketed layouts as the
first trusted source, and logical-partition-vs-physical-slot separation. Where the two
documents differ:

- **Ordering.** That draft's MVP starts at storage (bucketed `pin_table`); this one
  starts at plan-level propagation (its "later work" item "distribution propagation
  through hash join outputs"), because Phase 0 shows the join→group-by pattern is the
  cheapest measurable win and needs no storage or metadata contract at all.
- **Evidence.** This doc contributes the baseline measurements (engagement rates,
  N=1 degeneracy and its build-side-decides root cause, 87%-of-scan-output volume,
  11–20% engaged-regime share) that the other draft lists as a prerequisite for setting
  targets.
- **Scope of the descriptor.** The parallel draft's `distribution_spec` (versioned
  algorithm identity, null policy, external transforms) is the right long-term shape;
  this doc's `partitioning_info` is its minimal Phase-1 subset. Merging the two
  documents should keep the richer spec and this phasing/measurement section.

## Reproducing Phase 0

- Runner: views over `tpch_parquet_sf30` + `scripts/tpch-queries-run.sql` (22 queries,
  drop the duplicate line 23), 2 runs each, `SIRIUS_LOG_LEVEL=trace`.
- Configs: defaults vs `operator_params.hash_partition_bytes: 64Mi`.
- Analysis: `python3 tools/log_analyzer/parse_logs.py <log>`; sum
  `execution_time_ms` per `operator_type` from each query's `task_outputs.csv`;
  partition counts from `determined N partitions` debug lines.
- Caveats: single GPU (no transfer cost measured); scans re-read parquet each query
  (66–72% of op time), so shuffle shares of *non-scan* time are the relevant figures
  for pinned deployments.
