# GROUPJOIN framework campaign — handoff

State of the `feat/groupjoin-framework` branch as of 2026-08-26, written so an agent (or human)
with no prior context can pick the work up. The **normative spec is
[`groupjoin-framework-design.md`](../../groupjoin-framework-design.md)** at the repo root — it
was drafted from the two canonical groupjoin papers (Moerkotte & Neumann PVLDB 4(11);
Fent & Neumann PVLDB 14(11)), adversarially reviewed, and amended by the architect after every
PR, so it reflects what was actually built (each deviation is a line in its
"Design review resolutions" section). Read it before touching the code.

## What was delivered

The dense count join was generalized into a GROUPJOIN operator framework
(`GROUP_JOIN`, `src/op/sirius_physical_group_join.cpp`, kernels `src/cuda/group_join_impl.cu`,
detection ladder in `src/planner/sirius_plan_aggregate.cpp`), with three planner-live TPC-H
pathways and streamed execution:

| Commit | PR | Content |
|---|---|---|
| `45228fc9` | — | design doc |
| `34e7d39a` | PR-1 | mechanical generalization; count bundle only; P0 ladder rung verbatim |
| `5d1e5f08` | PR-2 | SUM/MIN/MAX/AVG slot policies; INNER + DIRECT forms; argument-validity gate; sparse mask-preserving fallback (executor-only) |
| `96cf8f2c` | PR-3 | P1 rung (q17: AVG per delim key over INNER join) behind `enable_group_join`; delim wiring; **preserved-port membership-filter publication**; default flipped on |
| `193ee829` | PR-4 | P2 rung (q2: MIN as single-input DIRECT over an opaque join-rooted child); single-child pipeline wiring; dense-forcing DIRECT reachability test |
| `ef272c01` | PR-5 | BUILD_STREAM (§4.8.1): schedule {ONE_SHOT, STREAM} selected by the counted-byte gate; per-batch accumulate tasks; plan-time hard-bound overflow/NOT-NULL proofs; per-role memory charges |
| `59ee2581` | PR-6 gate | **NO-GO recorded** (see below) |

Every PR went through a developer → reviewer(loop) → architect cycle; all reviewer
blockers/majors were fixed and re-verified; every architect audit ended approved.

## Performance reality (be honest with yourself before "improving" this)

- **SF1000 is neutral by construction and by measurement.** q13/P0 is SASS-instruction-identical
  to the pre-refactor binary (verified per PR, 24/24 kernel roles × 8 archs —
  `evidence/pr*/sass-parity.md`). q17 fuses streamed-dense in the *filtered* regime (the
  published membership filter cuts the counted side 6.0B → 6.0M rows) but is perf-neutral:
  suite A/B −2.4% vs isolated ×9 +3.0% — a sign conflict inside noise; q17 at SF1000 is
  scan/decode-bound, the deleted fragment was not on the critical path. q2 fuses
  streamed-sparse, neutral inside its 13–28% swing. 22/22 results byte-identical everywhere.
- **SF100 has the real wins**: q17 0.178 → 0.134 s, q2 0.184 → 0.174 s (one-shot regime).
- **Memory is the concrete PR-5 gain**: streamed accumulate tasks reserve at the 1 MiB floor
  instead of the old ~17×-input one-shot charge; the dense state (4.0 GB at SF1000 for the AVG
  bundle) is the one fixed residency.
- Load-bearing microbenchmark (`tools/atomic_bench.cu`, results in
  `tools/atomic-bench-results.txt`): GB300 does ~21.2 G scattered atomics/s over a 3.2 GB
  state region → an *unfiltered* 6B-row dense pass costs ~565 ms, more than all of today's
  q17 (272 ms). Filter parity is the entire SF1000 game for INNER shapes.

## PR-6 (q18 / `preserved_remap`): measured NO-GO

The design's §5.4 decision gate was executed 2026-08-26 (`evidence/pr6-q18-profile-REPORT.md`):
the q18 fragment a groupjoin would delete measures **~6 ms** against the ≥50 ms threshold; 90%
of q18's operator time is the scan-direct `sum GROUP BY l_orderkey HAVING` feeder (6B rows →
1.5B groups, 2.9 s in one HASH_GROUP_BY), which is not a groupjoin shape and is outside every
domain strategy (~6e9 dense domain; 1.5B keys ≫ the 128M dictionary-cap precedent). The
`preserved_remap` seam stays dormant until the membership sibling or q20 justifies it.

## Open items, in priority order

1. **Full unit suite (the one unexecuted merge gate, machine-policy-reserved for Kevin):**
   `pixi run make test` — on the GB300 workstation expect ~5/2692 known machine-specific
   timeout flakes; verify suspects in isolation, don't treat small counts as regressions.
2. **PR-5 carried review minors** (all documented in the PR-5 evidence index): (a) a narrow
   window where the dense build-claim converts a retryable OOM into a query failure (falls back
   to CPU — correct but slow); (b) two stale wording spots about hint/pop lock-serialization;
   (c) OOM/property tests are blind to cudf-internal scratch; (d) no concrete re-measure
   trigger defined for the q17/q2 SF1000 win claims (define one before claiming wins in any
   PR description); (e) one evidence-label overclaim + design typo.
3. **Future optimization seeds surfaced by the q18 profile** (outside this framework):
   dense-int64-key aggregation strategy (radix/sort groupby) for the 2.9 s q18 feeder;
   DYNAMIC_FILTER application cost on the lineitem probe scan (311 ms, q18's #3 operator).
4. **Unbuilt named seams** (all specified in the design doc, none blocking): `preserved_remap`
   + carried columns (§4.4/§5.4), value-aggregates-over-OUTER output masks (§4.5), multi-slot
   fusions, composite keys/q20, the mid-stream dense→sparse escalation seam (§4.8.1), the
   SEMI/ANTI dense-membership sibling operator (§4.3).

## How to work on this

- Build/test: `pixi run make`; focused tags
  `pixi run build/release/extension/sirius/test/cpp/sirius_unittest "[group_join]"`
  (also `[config]`, `[dynamic_filter]`, `[tier_narrowing_policy]`,
  `[compressed_schema_propagation]`).
- **GPU discipline (GB300 box):** exactly one GPU process at a time — `sirius_unittest`
  initializes full GPU pools for ANY tag; never run it concurrently with a benchmark. Never
  `LOAD sirius` in `build/release/duckdb` (statically initialized). In-memory DuckDB falls back
  to CPU; use file-backed DBs or parquet views.
- **Measurement:** only the `bench/sf1000-repro/` kit regime transfers
  (`DATA=<parquet dir> NAME=<name> pixi run bash bench/sf1000-repro/run.sh`; patched libcudf
  via LD_PRELOAD — machine-local at `~/cudf-src/cpp/build/libcudf.so`, rebuildable with
  `bench/sf1000-repro/build-libcudf.sh`). SF1000/SF100 parquet are machine-local under
  `test_datasets/` (gitignored); regenerate with `test_datasets/tpchgen-rs` (tpchgen-cli).
  Suite noise ~±1.2%; q7/q8/q10/q19 swing 13–28%; q2 swings 0.19–0.58 s — use ×9 single-query
  runs for q17/q2 claims. `tools/kit_run_trace.sh` runs any query subset with
  `sirius_log_level=trace` for the log analyzer (`tools/log_analyzer/parse_logs.py`).
- **Count-path changes:** any edit near `src/cuda/group_join_impl.cu` must re-verify SASS
  parity with `tools/sass_parity.py` (has `--self-test` and `--allow-new-roles`) against a
  pre-change `cuobjdump --dump-sass` of the extension (get cuobjdump via
  `pixi exec -s cuda-cuobjdump cuobjdump`). The R1 contract (design §7) is non-negotiable.
- **Knobs:** `operator_params.enable_dense_count_join` (P0, default true),
  `operator_params.enable_group_join` (P1/P2, default true since PR-3); engine-owned
  `group_join_max_state_bytes` = min(16 GiB, device/16) and the counted-byte schedule-selector
  gate = device/24, both with `SIRIUS_ENABLE_TEST_OPTIONS` SQL hooks. One config bool per
  pathway family returns planning to bit-identical-to-before behavior.

## Evidence map (this directory)

`evidence/pr{1..5}/` — per-PR merge-gate indexes (each row: gate, verdict, artifact) plus SASS
parity, allocation-set parity, and plan-parity reports. `evidence/pr6-q18-profile-REPORT.md` —
the NO-GO profile. `tools/` — SASS parity harness, scattered-atomic microbench (+ results),
trace-mode kit runner. Notes: these files were written against a session scratchpad; ignore
dead `/tmp/...` paths inside them — the relocated copies here are canonical. Raw kit run dirs
(`test/tpch_performance/output/tpch_20260825*`, `*20260826*`) and the pre-refactor baseline
binary/SASS existed only on the GB300 box and are reproducible from commits `db7bcb3c` (last
pre-refactor) vs branch HEAD.
