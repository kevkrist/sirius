# PROTO-C validation plan — P8 dynamic-filter membership fold (branch proto-c-df-fold)

Base: 1ad67e7f. Scope delivered: the fold + its gather-work metric only (amendment C2 — the
admission/reservation headline k=3->8 / 2.4->0.6 GB belongs to PROTO-B's estimator surface and is
only measurable stacked with PROTO-B).

## What changed

- `src/op/scan/duckdb_native_gpu_ingestible.cpp` — `post_filter_and_project` gains a gated
  dynamic-filter membership fold in its filter-evaluation branch, plus the new free function
  `fold_membership_probes_into_mask`.
- `src/include/op/scan/duckdb_native_gpu_ingestible.hpp` — declaration + docs for the fold
  function; refreshed the (previously "no read-time dynamic path") doc on
  `duckdb_native_ingestible_table_info::sirius_dynamic_filters`.
- `test/cpp/scan/test_duckdb_native_df_fold.cpp` (+ CMakeLists TEST_SOURCES entry) — new unit
  tests, tag `[duckdb_native_df_fold]`.
- No estimator function was touched (C2). No excluded file was touched
  (`dynamic_filter_merge.cpp`, `sirius_dynamic_filter.*`, DYNAMIC_FILTER operator). No
  expression_evaluator change was needed: the fold takes the static mask from the PUBLIC
  `evaluate()` (documented: one output column per expression; for the singular filter expression
  that column IS the mask `select()` gathers with).

## Mechanism and gate (C4/C5/C6)

Inside the existing `_filter_expression && state != ROW_FILTERED` branch (the declined-pushdown
branch, ingestible.cpp:365 at base), the fold fires only when ALL hold:

1. a static pushed-down filter exists (that branch's own condition),
2. `survivors == nullptr` (no late-materialization deferral is consuming this call — C6),
3. the channel is wired (`_info->sirius_dynamic_filters != nullptr`; q1/q6 have a null channel,
   so their cost is one pointer null-check),
4. `has_filters()` — a single atomic load, never a wait (unpublished => today's path, no
   ordering change),
5. the snapshot (`snapshot_membership_probes`, header reuse per C1) attaches >= 1 mask-capable
   probe (zone-map-only channels fall through),
6. the split has > 0 rows.

Fold: static mask from `evaluate()`; each probe closure (the publishing filter's own
`compute_mask` — C4, no hand-rolled probes; a null return = incompatible probe = skipped exactly
as the downstream cascade skips it) runs on input column `slot` (the
`snapshot_membership_probes` mapping invariant: slot i = output position i); masks are combined
with `cudf::binary_operation LOGICAL_AND` (null propagates, and `apply_boolean_mask` drops null
entries — identical drop set to the downstream cascade of per-mask applies); ONE
`apply_boolean_mask` gathers the output columns (same `[0..output_arity)` projection fold as the
`select()` path). Every non-fold condition takes today's code bit-for-bit (C5) — the
`exec.select(...)` calls are untouched.

C6 detail: this ingestible's `can_report_survivors()` is false, so
`install_late_materialization` refuses any deferral on a native scan that has a row filter
(sirius_scan_manager.cpp:839-868); since the fold additionally requires the static filter, a
deferring scan can never reach it — and the explicit `survivors == nullptr` guard makes that
structural rather than incidental. A DF-wired scan WITHOUT a static filter (which late-mat does
admit, relying on "the batch IS the chunk") is gated out by condition 1, so the fold never
compacts a batch a deferral believes row-preserving. Covered by the "without a static filter"
unit test.

Sync accounting: the fold path replaces `select()`'s single internal `apply_boolean_mask`
(sizing sync) with its own single `apply_boolean_mask` — net host-sync count unchanged on the
task path. The probes and the LOGICAL_AND enqueue only.

## ORCHESTRATOR: run the byte-identity gate FIRST

Byte-identity is AT RISK by contract (gather composition changes floating-point arrival order
downstream). In principle the fold emits the same rows in the same relative order per batch
(conservative prefilter; the downstream DYNAMIC_FILTER op re-applies and the FILTER op is exact),
but this MUST be proven before any benchmarking:

1. Small config first: TPC-H SF1 (or the smallest SF with q19/q14 DF wiring), duckdb-native
   format, nightly-regime env (SIRIUS_EXP_FUSED_SCAN_FILTER=1, SIRIUS_EXP_LATE_MAT=1, ast_jit),
   q19 + q14 + q1 + q6 + the full 22-query result comparison vs the 1ad67e7f baseline binary.
2. Then the SF1000 A/B regimes below.
3. If ANY result differs: STOP and escalate to the hunt lead — do not proceed to benchmarks, do
   not attempt fixes that reorder rows.

Expected benign behavior change (review C4, log-visible, NOT a failure): the downstream
DYNAMIC_FILTER operator observes keep ~1.0 after the fold and its gate disables itself
(`[apply_dynamic_filters] selectivity gate: kept ... -> DISABLED`). That is the excluded
component's own adaptive decision reacting to already-filtered input; its code is untouched.

## Unit tests (written, NOT run — GPU)

```
pixi run build/release/extension/sirius/test/cpp/sirius_unittest "[duckdb_native_df_fold]"
```

| test | proves |
| --- | --- |
| published membership probes fold into the static filter's gather | hit/miss conjunction through the real ingestible path |
| an all-keys membership probe folds to exactly the static-only rows | fold is row-conservative when membership is useless |
| an unpublished channel falls through to the static-only path | C5 unpublished fall-through, non-blocking |
| a channel with no mask-capable filters falls through | zero-attached-probes gate (zone-map-only channel) |
| without a static filter a published channel drops no rows in the scan | C5 static-filter gate + C6 late-mat row-preservation |
| the fold gathers only the output columns when projection is required | pure-filter column elision preserved at combined selectivity |
| fold ANDs real probe closures with null-as-drop semantics | C4 NULL semantics == downstream cascade |
| fold skips a probe that declines its key column | null-return (incompatible carrier) skip |

Also run the neighboring suites the change could disturb:
`"[scan][duckdb_native_empty_split]"`, `"[scan][duckdb_native_host_backed]"`,
`"[dynamic_filter][scan_merge]"`.

## C3 pre-check: confirm q19-r2 actually reaches the fold's branch

Before pricing, confirm from diag logs that q19-r2 lineitem chunks take the declined-pushdown
branch (state != ROW_FILTERED) where the fold lives: with
`SIRIUS_DECOMPRESSION_PUSHDOWN_DIAG` / debug logging on, look for
`selection_unprofitable` latching (prepare_for_processing) followed by my fold line
`[duckdb_native_gpu_ingestible::post_filter_and_project] dynamic-filter fold: probes=...
rows A -> B` on the same tasks. If chunks instead decode ROW_FILTERED, the fold is on a cold
path for that regime and the A/B below will show ~0 — report that rather than forcing it.

## A/B measurement plan (after identity passes)

Standalone PROTO-C metric = gather-phase work only (C2):

- R2' (nightly regime, compressed gpu-tier pins, SF1000): q19 + q14, 3+ blocks each, vs the
  1ad67e7f baseline binary. Contract expectation: the q19-r2 figure of merit -0.15..-0.17 s came from
  fold+admission COMBINED; standalone expect the gather/GPU-work share only — compare the scan
  phase (decode+post_filter) time and the disappearance of the downstream DYNAMIC_FILTER
  gather (its `[apply_dynamic_filters] ... apply: X -> Y rows` lines should vanish or go
  keep~1.0/DISABLED after the first batches).
- Gather-work evidence from logs: my fold line reports `rows A -> B` per split; baseline logs
  show post_filter gathering at static selectivity (A -> ~0.25A) and the DF op gathering again
  (~0.25A -> ~0.01A). Fold run: one gather A -> ~0.01A.
- Probe-cost check (must stay net-negative): the fold probes FULL split rows (~4x the rows the
  downstream op probes post-static-gather; P8 prices 12 -> ~48 ms GPU). Verify via nsys or the
  per-op phase times that q19/q14 scans did not regress.
- R1' q19 rider: same binary, R1' regime, report only.
- q1/q6 sanity: no fold lines in logs (null channel), timing unchanged within noise.
- Admission/reservation claims: measure ONLY stacked with PROTO-B (leave-one-out per the
  cross-contract rule); do not attribute k=3->8 to this branch alone.

## Interaction with drafts #1476/#1661 (decoder rewrites)

Those drafts rewrite the decoder/ingestible wholesale, including this file (MAP 4.2 flags
`duckdb_native_gpu_ingestible.cpp` as rewritten). The fold is confined to
`post_filter_and_project` + one new free function + the hpp doc/declaration, per X3, so a rebase
onto either draft must re-home the fold into whatever replaces `post_filter_and_project`'s
filter-evaluation branch; the fold function itself (mask x snapshot -> mask) is
decoder-agnostic and should survive as-is. No coalescer, ctor-fallback, or hang-invariant code
was touched (X3).

## Adjacent wins noted, not implemented (per common rules)

- Review C5 suggested an adaptive keep-rate guard of the fold's own (beyond the binding
  static+published gate implemented) for DF-wired scans outside q19/q14 where probes are
  useless. The cheapest mechanism: an operator-shared latch like
  `pushdown_selection_unprofitable`, set when a folded batch's membership marginal keep-rate
  is ~1.0 — but measuring that marginal needs one extra count readback per batch. If the A/B
  shows regressions on other DF-wired scans, add the latch; the downstream op's own per-filter
  gate already bounds repeated useless work there.
- The parquet ingestible's post_filter path has the same fold opportunity (out of scope: native
  surface only this wave).
