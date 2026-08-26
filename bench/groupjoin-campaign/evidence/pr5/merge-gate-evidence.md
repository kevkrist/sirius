# PR-5 merge-gate evidence index (design `groupjoin-framework-design.md` section 9, PR-5 row)

All artifacts live under this `pr5/` scratchpad directory unless noted. Candidate = the PR-5
working tree (uncommitted) on branch `feat/groupjoin-framework` @ 193ee829 (PR-1..PR-4 merged),
built at `build/release/`; every gate below was taken on the FINAL post-clang-format binary.
Labels: **A = enable_group_join OFF** (`../pr4/sirius-groupjoin-off.yaml`), **B = ON / default**
(`../pr4/sirius-groupjoin-on.yaml`).

| gate | status | artifact |
|---|---|---|
| SF1000 kit A/B | PASS with the honest q17 performance verdict below. **q17-B FUSES with the streamed schedule** -- log evidence in `tpch_20260825_235539_gj_pr5_sf1000_B/sirius/q17/sirius.log` and the x9 runs: plan-time proofs (`facts for read_parquet.l_quantity: not_null=true bounds=true [100, 5000] row_bound=5999989709` -- 6.0e9 x 5000 = 3.0e13 << 2^63, the section 4.8.1 numbers exactly), `Fusing INNER AVG into GROUP_JOIN (STREAM schedule) ... membership publication installed`, plan render `port: counted, barrier: PIPELINE` / `port: preserved, barrier: FULL`, `STREAM build: INNER AVG committed dense on device 0: keys in [1890, 199998879] (range 199996990, presence 32-bit, matched 64-bit), preserved rows 200585` (the 200M-slot / 20 B = 4.0 GB state), `STREAM emit: 200585 group rows (dense strategy, 6013896 counted rows)` -- 6.0 M counted rows against a 6.0 B-row scan means the **membership filter landed before the accumulates (filtered regime)**. **q2-B fuses streamed-sparse**: `Fusing DIRECT MIN into GROUP_JOIN (STREAM schedule)` + `STREAM build: DIRECT MIN committed sparse (merge ladder)` + `STREAM emit: 471301 group rows (sparse strategy, 638799 counted rows)` -- the PR-4 decline dissolved; the ~1000x child estimate error (800 M est vs 638,799 actual) selected a schedule instead of sizing a reservation. Plans: 20/22 identical A/B; only q2/q17 differ, by their knob-gated fusions (q13's P0 marker identical; q21/q22 verified in a dedicated pair after a midnight log-rotation carve quirk). Results: 22/22 `result.txt` byte-identical A/B | `sf1000-plan-parity.md`; run dirs `tpch_20260825_235103_gj_pr5_sf1000_A`, `tpch_20260825_235539_gj_pr5_sf1000_B`; raw logs `kit-sf1000-A.log`, `kit-sf1000-B.log` |
| SF1000 q17 performance (the -10..25% claim) | **Claim NOT met; floor (neutral-if-scan-bound) met; honest-failure clause NOT invoked** -- see the verdict section below for the numbers and the judgment | x9 dirs `..._gj_pr5_sf1000_q17_Ax9`, `..._q17_Bx9`, `..._q17_Ax9b`, `..._q17_Bx9b` |
| SF1000 q2 | Fuses streamed-sparse; neutral within q2's historical swing: suite best-of-3 A 0.1946 / B 0.2113 (+8.6%); x9 best A 0.1653 / B 0.1626 (-1.6%), median A 0.1985 / B 0.2051 (+3.3%). q2's documented swing class is 13-28%. **RE-TAKEN 2026-08-26 after the review fixes (F1 emit rewrite + F2 charges touch q2's streamed-sparse path)**: two independent x9 pairs on the post-fix final binary -- pair 1 warm median A 0.2038 / B 0.2198 (+7.9%), pair 2 A 0.2050 / B 0.2166 (+5.7%), pooled warm means A 0.2024 / B 0.2125 (+5.0%); B holds the single fastest iteration of all four runs (0.1431 vs A best 0.1645), so the channels flip sign again -- still neutral within the swing class. q2 still fuses `DIRECT MIN ... STREAM ... sparse (merge ladder)`, `STREAM emit: 471301 group rows (sparse strategy, 638799 counted rows)`, and result.txt is md5-identical A/B and to the prior gate's A run (5ec23ca3...) | prior x9 dirs `..._gj_pr5_sf1000_q2_Ax9`, `..._q2_Bx9`; re-taken dirs `tpch_20260826_151014_gj_pr5fix_sf1000_q2_Bx9`, `..._151357_..._Ax9`, `..._152209_..._Bx9b`, `..._152349_..._Ax9b`; logs `kit-sf1000-q2-{A,B}x9{,b}-fix.log` |
| SF100 kit A/B | PASS -- q17 and q2 keep fusing **one-shot** (the schedule selector: `Fusing INNER AVG into GROUP_JOIN (one-shot schedule)` with the PR-3 `INNER AVG sparse path` filtered regime; `Fusing DIRECT MIN into GROUP_JOIN (one-shot schedule)` with PR-4's exact `DIRECT MIN sparse path: state bytes 239997456, input bytes 1585368`). Not worse than PR-4: q17-B 0.1307 (PR-4: 0.1341), q2-B 0.1685 (PR-4: 0.1739), best-of-3. Plans 20/22 identical A/B (only q2/q17 fusions differ); results 22/22 byte-identical | run dirs `tpch_20260826_000718_gj_pr5_sf100_A`, `tpch_20260826_000943_gj_pr5_sf100_B`; `kit-sf100-A.log`, `kit-sf100-B.log` |
| knob-off-vs-PR-4-B plan parity (PR-4 carried minor) | PASS -- PR-5-A (knob off) vs PR-4's `tpch_20260825_190445_gj_pr4_sf1000_B`: every structural plan line (pipelines, wirings, operators, scans) identical 22/22 and all 22 results md5-equal. The raw diff flags q2/q17 only because PR-4-B's logs carry its own "fusion declined" INFO lines (knob-on planner chatter a knob-off run never emits) -- i.e., "identical" means identical plan structure and results; knob-gated log chatter is definitionally absent from A | `sf1000-plan-parity.md` (section 2) |
| count-kernel SASS parity vs ORIGINAL baseline | PASS -- harness self-test PASS (baseline-vs-baseline clean; single-instruction mutation detected); 24/24 baseline count-kernel roles instruction-identical across 8 archs; 81 candidate-only roles (identical count to PR-4: the streamed dense state reuses the PR-2 value-kernel instantiations, zero new device kernels). Taken on the final binary. **RE-VERIFIED 2026-08-26 on the post-review-fix final binary**: `group_join_impl.cu` and its includes untouched and not recompiled (0 mentions in `build-final.log`); the full `cuobjdump --dump-sass` of the re-linked extension is byte-identical to the dump behind this PASS (`cmp sass-final.txt sass-final-relink.txt`) | `sass-parity.md` (baseline `../baseline/sass-full.txt`, candidate `sass-final.txt`, harness `../pr2/sass_parity.py`); re-check `sass-final-relink.txt` |
| focused test tags (final binary) | PASS -- **re-taken 2026-08-26 on the post-review-fix final binary**: `[group_join]` 98 cases / 174857 asserts (`test-group_join-final3.log`; +5 cases are the new section-10 tests), `[config]` 65/713 (`test-config-final3.log`), `[dynamic_filter]` 227/2213, `[tier_narrowing_policy]` 13/111, `[compressed_schema_propagation]` 12/238, `[gpu_pipeline_executor]` 3/21 (`test-*-f3.log`) | |
| pre-commit | PASS on every changed file (clang-format reformatted twice mid-stream; all gates re-taken on the post-format final binary). **Re-run 2026-08-26 after the review fixes: all hooks pass** | |
| reservation-profile evidence | PASS -- TRACE-level per-task reservations of the streamed q17 GROUP_JOIN pipeline (dedicated run `tpch_20260826_003343_gj_pr5_trace_q17`): exactly one build task at **16,764,696,720 B** (the 16,760,438,784 B budget + 4 x preserved + floor; transient), **16 accumulate tasks at 1,048,576 B each** (the 1 MiB floor -- per-batch tasks are small, which is the point), and one emit task at **4,011,468,972 B** (the 4.0 GB state-again CUB proxy + group-bounded outputs). No ~17x-input (~1.7 TB) charge exists anywhere -- the reservation class the one-shot schedule could never have scheduled. **RE-TAKEN 2026-08-26 after F2's build-charge change** (run `tpch_20260826_152803_gj_pr5fix_trace_q17`): one build task at **16,774,525,450 B** (budget + the structural preserved-partial bound, +9.8 MB over the old 4x-preserved term on a 16.76 GB transient charge), the same **16 accumulates at the 1,048,576 B floor**, and the emit **identical at 4,011,468,972 B**; q17 result.txt md5-identical to the knob-off reference (14491bcf...) | `trace-q17-reservations.txt`; re-taken `trace-q17-reservations-fix.txt` |

## The q17 SF1000 performance verdict (numbers as-is)

- Suite best-of-3 (the section 9 gate's own instrument): A 0.2705 s, B 0.2641 s (**-2.4 %**, inside
  the +-1.2 % suite noise band -> neutral).
- 9-iteration single-query pattern, two independent pairs (warm iterations 2-9):
  pair 1 A best/median 0.2503/0.2670 vs B 0.2645/0.2788; pair 2 A 0.2539/0.2684 vs B
  0.2627/0.2742. Pooled warm means: A 0.2685, B 0.2766 (**+3.0 %**, ~8 ms; overlapping
  distributions, consistent sign).
- Judgment: the -10..25 % estimate did NOT materialize; the measured regime is the doc's floor
  case -- the membership filter lands (6.0 M of 6.0 B rows accumulate, ~1.5 ms-class), so q17
  stays scan/decode/delim-bound and the deleted fragment is small, while the streamed dense
  state's fixed costs (4.0 GB memset + the emit's state-sized selection scan, single-digit
  ms/GB -- exactly the bounded regret section 4.8.1 accepted when it made gates (d)/(e)
  one-shot-only) cost ~8 ms on the single-query pattern. The two measurement channels disagree in
  sign (-2.4 % suite vs +3.0 % x9), so "regresses beyond noise" is not established and the
  honest-failure clause was not invoked; INNER stays in `group_join_stream_forms`. Containment
  remains one engine-owned knob away (`SET group_join_stream_forms = 'DIRECT'` restores the
  PR-3/PR-4 decline for over-gate INNER shapes, unit- and integration-tested).

## Correctness cross-checks beyond A/B

- Streamed q17 result at SF1000 is byte-identical to the knob-off result
  (md5 14491bcfc24569f1..., smoke dir `tpch_20260825_231926_gj_pr5_smoke5_q17` vs the A suite run).
- One-shot-vs-streamed result parity across the whole bundle matrix is Catch2-enforced
  (multi-batch streams incl. out-of-range and pre-filter batches, adversarial-extrema sparse
  INNER, DIRECT ladder with NULL keys/arguments; `test/cpp/operator/test_physical_group_join_stream.cpp`).

## Stats-plumbing note (the architect's risk 4, resolved in-session)

DuckDB's multi-file parquet binding surfaces no column statistics (`MultiFileScanStats` returns
nullptr for MULTIPLE_FILES without union_by_name) and its cardinality is estimate-only (no
`has_max_cardinality`), so the kit's `read_parquet` views would have failed the proofs closed.
The proofs therefore read the parquet footers themselves through the new
`sirius_scan_manager::describe_parquet_metadata` (footer parsed once per file per process, served
from the ioctx metadata store the pin phase already populates -- zero extra IO in the kit).
NOT-NULL evidence is schema-level REQUIRED repetition OR zero `null_count` on every column chunk
(both channels needed: cudf's footer parse reports the repetition of these files as non-REQUIRED
even though pyarrow shows `required`, so the null-count channel is what fires); the row bound is
the exact summed `num_rows`; value bounds decode INT32/INT64 chunk statistics. Native tables use
`statistics_extended` (the callback DuckDB actually sets for `seq_scan`) plus the
`has_max_cardinality`-flagged exact row count.

## Addendum 2026-08-26: strict-review findings F1-F4, all fixed

All four findings verified real against source and fixed; every invalidated gate re-taken above.

- **F1 (blocker, emit destroyed-before-published)**: CONFIRMED -- the sparse emit moved every
  ladder partial out (+cleared) and moved `preserved_partial` before the collapse/combine/
  finalize allocations; an OOM there converts to `oom_reschedule_exception` and the replay
  emitted an empty aggregate (DIRECT) or dereferenced the moved-from preserved partial (INNER).
  Fix: the collapse now merges the resident slots sequentially **by view** into a task-local
  table (single-resident ladders deep-copy), `sparse_inner_combine` takes the preserved partial
  as a `table_view`, and `release_stream_state()` at the end of emit is the emit's only state
  mutation. Regression test: "OOM-rescheduled emit replays against unreleased state" sweeps the
  failure point k = 0..success over DIRECT and INNER; mutation check re-introduced the
  destructive collapse and both sections failed on `rows == oracle` (`test-mutation-detect.log`).
- **F2 (major, sparse accumulate charge under-covers the carry collapse)**: CONFIRMED
  empirically -- at the property test's 3-deep full collapse the old charge (4xB + 2x(R+B) ~=
  46.4 MB) is exceeded by the observed 60.8 MB task peak. Fix: the charge now simulates the
  exact deterministic carry chain over the measured resident-slot sizes (`sparse_fold_peak_bytes`;
  the in-flight <= 1 pop gate makes the ladder stable between pop and fold), the sparse emit
  charge simulates the non-destructive collapse (`sparse_collapse_peak_bytes`), and the INNER
  build charge replaces 4x-preserved with a per-row structural groupby bound. Property test
  extended to every sparse role (DIRECT build/accumulates incl. the full collapse/emit; INNER
  build/accumulates/emit-with-combine) via a statistics adaptor injected through the new
  `set_stream_memory_resource_for_testing` seam; margins on the final binary: accumulate
  15.4/4.0 MB, carry steps 30.1/15.2 and 51.7/30.4, full collapse 94.9/60.8, emit 65.4/19.2 MB
  (`test-property-margins.log`). Design table rows updated in the same change.
- **F3 (major, counted_rows_total lost on sparse accumulate OOM-retry)**: CONFIRMED -- the claim
  latch was consumed before the fold but the row accounting was gated on `first_application`
  after it, so a mid-fold OOM's replay folded correctly yet never recorded the rows, weakening
  emit-time COUNT product validation and the dense row-bound belt-check. Fix: the accumulate now
  validates and harvests all columns first, records the rows (and runs the row-bound belt-check)
  immediately after the claim and **before** the fold, then folds. Regression test:
  "OOM-rescheduled sparse accumulate replays once with its rows recorded" checks
  `stream_counted_rows_for_testing() == total rows` after every (possibly replayed) batch and
  that the pin rides the input unchanged; the mutation check restored the old ordering and the
  test failed on the row total (`test-mutation-detect.log`).
- **F4 (major, missing section-10 PR-5 tests)**: CONFIRMED and delivered in
  `test/cpp/operator/test_physical_group_join_stream.cpp`: emit OOM-retry (F1's regression),
  build OOM-retry (dense and sparse commits, k-sweep, partial-state rollback), sparse accumulate
  OOM-retry (F3's regression, pin carried), mid-stream spill (counted batches forced to HOST via
  `convertible_data_batch` between arrival and accumulate, re-materialized per task through
  `prepare_for_processing`, oracle parity), and the per-role sparse reservation property test.
  New tags: `[oom_retry]`, `[spill]` (both under `[group_join][stream]`). Final tally 98 cases /
  174857 assertions, all passing (`test-group_join-final3.log`).

Test seams added for these (documented as such in the header): stream_state's injectable memory
resource (`set_stream_memory_resource_for_testing`) and the `stream_counted_rows_for_testing`
accessor; `operator_test_utils.hpp` gained `get_default_memory_manager()`/
`get_default_host_space()` (same static manager as before). The OOM injector shares its
countdown across copies because `rmm::device_buffer` type-erases the resource by value.
