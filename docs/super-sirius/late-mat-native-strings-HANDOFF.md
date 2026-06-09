# Late materialization for the DuckDB-native string decode — handoff

**Branch:** `kk/late-mat-native-strings`  ·  **Base:** `dev` @ `f1eb9e02`  ·  **Status:** prototype
complete + validated + **evaluated end-to-end → net-zero on TPC-H, PARKED (see FINAL EVALUATION below).**

This note is the pick-up-where-I-left-off for the late-materialization prototype. It records what is
done, what is proven, what is *not* yet done, and the exact commands to resume on another machine.

---

## FINAL EVALUATION — net-zero on TPC-H (2026-06-09; supersedes the "1.4–1.8× win" headline below)

**Bottom line: the kernels are correct and the optimization works, but it yields ~no end-to-end
benefit on TPC-H, so the work is parked here.** The 1.4–1.8× numbers in the session updates below are
real, but they were measured on **synthetic single-table probes** —
`SELECT <wide FSST col> FROM <big table> WHERE <fixed-width col> = <very low selectivity>` — hand-built
to be late-mat's ideal case. TPC-H does not contain that shape.

### The full 22-query sweep
Only **3 of 22** canonical TPC-H queries engage late-mat at all; every other query runs 100% eager
(reconstructed via `run_tpch_latemat.sh`, counting `late_mat=true/false` splits per query):

| query | LM splits | projects | encoding | regime → why ~0 |
|-------|-----------|----------|----------|------------------|
| q1  | 26 | `l_returnflag`, `l_linestatus` | DICTIONARY (1-char, 2–3 distinct) | **~98% keep** — selection machinery is pure overhead |
| q4  | 5  | `o_orderpriority` | DICTIONARY (5 distinct) | grouped → 5 output rows; trivial payload |
| q12 | 18 | `l_shipmode` | DICTIONARY (7 distinct) | grouped → 2 output rows; trivial payload |

All three project **short, low-cardinality DICTIONARY** strings that get grouped/aggregated away — there
is essentially no decode work to skip. The A/B (`timeab_tpch.py`, q1/q4/q12) measured no end-to-end win.

### Why it nets to zero — three compounding reasons
1. **Triggers rarely, and only where the payload is trivial.** The 3 engaging queries project tiny
   low-card DICTIONARY strings (often under high keep%) → nothing to save.
2. **Where it WOULD help, the query shape blocks it.** Wide high-card FSST `*_comment` columns
   (Q10 `c_comment`, Q2/Q20/Q21 `s_comment`, Q13 `o_comment`) are exactly late-mat's sweet spot, but
   none engage: either the string is *itself* a filter (Q13 `NOT LIKE` → correctly declines), or the
   table's row reduction is **join-driven** — downstream of the scan — rather than from a scan-local
   fixed-width filter. Late-mat can only ride **scan-local fixed-width** selectivity; TPC-H prunes the
   string-bearing tables via joins/aggregations, which land after the decode already happened.
3. **The one big-table trigger has the wrong selectivity.** Q1 fires on all 26 lineitem splits, but its
   filter (`l_shipdate <= …`) keeps ~98% of rows → the selection cost (two binary searches/chunk,
   `apply_boolean_mask`, `index_seq` over all rows) dominates the ~2% of decode it avoids → break-even
   (the microbench's 100%-keep regression regime).

Even the synthetic wins were diluted: nsys showed **6× less kernel work → only 1.4–1.8× end-to-end**,
because IO + filter-eval + `apply_boolean_mask` + aggregation crowd out decode's share of wall-clock.
In a multi-join query decode is a single-digit-% slice, so any decode win rounds to nothing.

### Would other column types help? (forward note — FSST is not special)
benefit ≈ (per-row materialization cost) × (rows eliminated) − selection overhead − selection-independent prep.
- **Within strings, DICTIONARY beats FSST** (13× vs 6× at 1% keep, microbench). Width × selectivity
  drives it, not the codec.
- **Biggest theoretical win: wide variable-width / nested** (LIST/ARRAY/STRUCT/JSON/embeddings — KB/row).
  But the native decoder has **no nested support today** (it dispatches varchar vs fixed-width only), so
  this is "would be the prize if the scan supported it," not reachable now.
- **Wide fixed-width** (decimal128, UUID): modest, bandwidth-only — and fixed-width projected columns are
  already post-filter-compacted via `apply_boolean_mask`, so little extra to gain.
- **Narrow fixed-width** (int32, dates): ~zero (gather cost ≈ decode saved; and they're usually the
  *filter* column, which can never be late-materialized).
- **Sequential / cumulative codecs** (DELTA, DELTA_FOR, and FSST's compressed-offset *addressing*) have an
  irreducible all-rows prep floor → can't fully skip dead rows (this is why DICT_FSST stayed ~1.0×).

To make late-mat earn its keep you'd need a workload with (a) a wide var-width/nested column in the output
projection, (b) a selective **scan-local fixed-width** filter on the *same table*, and (c) little join/agg
dilution — i.e. log/event analytics (`SELECT payload FROM events WHERE ts BETWEEN … AND host=…`), not
TPC-H. Widening column-type support raises the *ceiling*; it does nothing about the *trigger rate* that
makes TPC-H net zero. **Decision: park the branch; revisit only if such a workload becomes a target.**

The optimization is correct and on by default (`late_materialize_native_strings`, default true) with a
small-split gate — it is never a correctness risk, only a no-op on TPC-H. The detailed build record and
the (synthetic) win numbers are preserved verbatim below for the record.

---

## Session update — 2026-06-09 (e2e validation + SF50 profiling, on the GB10 box)

Resumed sections A + B. Headline: **late-mat is correct end-to-end and is a real 1.4–1.8× win on
big-table low-selectivity FSST scans.** Two bugs found+fixed, one config requirement uncovered.

**Setup requirement (not a code bug, but blocks the native scan):** the GPU-native DuckDB scan needs
`scan_manager.use_sirius_datasource: true` in the sirius yaml. It defaults to **false**, and when
false `SiriusContext::initialize()` omits the local io_uring backend, so the scan_manager has no
backend for local `.duckdb` paths and `decode_duckdb_native_split` throws
`"missing io_ctx, io_obj, or SingleFileBlockManager"`. (Parquet falls back to
`cudf::io::datasource`; the native scan has no fallback.) Added it to `~/.sirius/sirius.yaml` under
`sirius.executor.scan_manager`.

**Bug #1 (FIXED) — empty selection crashes late-mat.** A query whose fixed-width filter selects **zero
rows** (e.g. `WHERE l_shipdate = DATE '1850-01-01'`) threw `cudf Column size mismatch: N != 0`. Cause:
`gpu_decode_strings_column` inferred `sel_active = (d_sel != nullptr)`, but the decoder derives `d_sel`
from a **zero-row** compacted index column whose device pointer is **null** → `sel_active` went false →
it decoded all `total_rows` instead of 0, mismatching the 0-row fixed-width columns. `{nullptr, 0}` was
ambiguous between "decode all" (eager default) and "empty selection". Fix: added an explicit
`bool active` to `string_decode_selection`; the kernel now gates on `selection.active`, not the
pointer. The selective test's empty case now passes a genuinely **null** d_sel (it used to pass a
non-null capacity buffer, which is exactly why it never caught this). Touched:
`gpu_decode_strings.{cuh,cu}`, `duckdb_native_decoder.cpp`, `test_gpu_decode_strings_selective.cpp`,
`bench_decode_codecs.cpp`.

**Correctness (section A) — DONE, all green.** Per-row diff (unique key + full string content, sorted)
vs pure-DuckDB CPU ground truth across 9 cases: FSST single/multi-varchar (PA/PD), FSST+Uncompressed
3-varchar + decimal filter (PE), DICTIONARY+FSST mixed + range filter (PF), compound 2-col fw filter
(PH), empty selection (PG), decline-on-varchar-filter (PI, late-mat correctly refuses), Q1 (DICTIONARY
~98% keep + aggregation), Q10 (multi-join, correctly declines). cpu==eager==latemat exact everywhere;
Q1's only CPU diff is a 1-ULP `avg_disc` float-summation-order artifact (present in eager too).
Harness lives in `test/tpch_performance/latemat_eval/` (`run_query.sh`, `compare.sh`, `timeab.py`).

**Performance (section B) — DONE.** Warm, interleaved A/B in one process (cold = process-init-dominated
on GB10). End-to-end query speedups at SF50:

| probe (single-table)                        | keep%  | late-mat speedup |
|---------------------------------------------|--------|------------------|
| PB lineitem FSST `l_comment` (1 month)      | ~1.5%  | **1.8×**         |
| PA lineitem FSST `l_comment` (1 day)        | ~0.04% | **1.7×**         |
| PD orders 2×FSST                            | ~0.04% | **1.4×**         |
| PF orders DICTIONARY+FSST                   | ~0.04% | **1.1×**         |
| Q1 DICTIONARY `l_returnflag/linestatus`     | ~98%   | **1.0×** (break-even) |
| PE customer 3×FSST (small 7.5M-row table)   | ~0.9%  | 0.9×             |

nsys attribution (PE): late-mat does **6× less GPU kernel work** (the `kernel_gather_fsst_chunked`
chars-gather collapses 32.3 M→0.78 M ns), exactly as the microbench predicted. The end-to-end wins are
smaller than the kernel 6× because IO (reading the compressed FSST bytes is unchanged) + filter-eval +
`apply_boolean_mask` + aggregation dilute the decode share.

**Codec reality at SF50 (matters for section C below):** DuckDB stores **every** wide-text column
(`l/o/c/p/s_comment`, `c_address/c_name/c_phone`, …) as plain **FSST**, and low-card columns as
**DICTIONARY**. **There is no DICT_FSST anywhere in the dataset.** So the "flat DICT_FSST ceiling" (old
section C) is *not reachable* with real TPC-H storage — the live codecs are precisely the two late-mat
accelerates well.

**The small-table regression is RMM pool pressure, not a late-mat flaw.** Under the conservative GB10
default (`gpu.usage_limit_fraction: 0.30`), PE was **0.19× (5× slower)** and bimodal. Warm nsys showed
late-mat's *non-kernel* time was dominated by `cudaMallocFromPoolAsync` (7.6 s vs 1.6 s over 6 iters) +
`cudaStreamSynchronize` (5.1 s vs 0.5 s, one sync stalling 339 ms) — its extra allocations (per-split
`index_seq` over all rows, `apply_boolean_mask` outputs, per-varchar selective buffers) + the **forced
per-column exact-total chars-sizing sync** thrash a tight pool. Raising `usage_limit_fraction` to 0.70
(via `SIRIUS_CONFIG_FILE=/tmp/sirius_highmem.yaml`) lifted PE to **0.90×** (min ON 0.070 s < OFF
0.091 s) and nudged every other probe up (PB 1.8×, PA 1.7×). So: late-mat wins scale with the avoided
eager-decode size; on small/cheap scans the fixed per-split + per-column overhead dominates, and a
tight pool turns that overhead catastrophic.

**Revised next-work priorities (supersedes old C/D):**
1. **[DONE — same session] Remove the forced per-varchar chars-sizing sync.**
   `gpu_decode_strings_column` used to force an exact-total D2H read-back (one `cudaStreamSynchronize`
   + `cudaMemcpy` per varchar column per split) under any selection, to size the chars buffer to the
   selected rows. Replaced with a **selection-aware sync-free upper bound** `out_rows *
   max(seg.max_string_length)` — the same over-allocation strategy the non-selection path already uses
   (guarded by the same `HOST_UPPER_BOUND_LIMIT`; falls back to the exact read-back only when a
   segment's length stat is unknown). One-line conceptual change in `gpu_decode_strings.cu` (track
   `max_seg_max_len`, re-bound `cum_chars_upper` under selection instead of `needs_exact_total=true`).
   Result (warm A/B, SF50): with an adequately-sized pool, **every firing probe improved** — PD
   1.36→1.88×, PA 1.71→1.85×, PF 1.12→1.61×, and **PE flipped from 0.90× (regression) to 1.28× (win)**;
   Q1 stays ~1.0×. Correctness unchanged (kernel `[selective]` + the full section-A matrix still green;
   the over-allocation is invisible because cudf strings bound on the offsets, exactly as the
   non-selection path already relied on). Sync time in the warm PE nsys dropped ~2×.
2. **[DONE — same session] Gate late-mat off for small splits.** New config
   `late_materialize_min_rows` (default **1,048,576** ≈ 8 row groups; yaml
   `executor.operator_params` + `SET late_materialize_min_rows`). Decided **per split** in
   `next_split_provider` (threads a `late_materialize` bool through `duckdb_native_split_payload`);
   `materialize_table` keys `filter_state` + the decoder's `lm_ptr` off it, and `apply_filter` flips in
   lockstep — so a gated-off split falls back to the eager decode-then-(post)filter path with no
   double/zero filtering. Validated: forcing the threshold above the split size puts every split on the
   eager path (`late_mat=false`) with **results still == CPU**; small tables (supplier, 500 K) and the
   partial tail split of multi-split scans (orders) gate off correctly while the big splits late-mat,
   all correct. At the default, **every firing probe is unaffected** (lineitem/orders/customer splits
   are all ≥ 1M), so the post-P1 win numbers stand. The knob is a tunable escape hatch: the only
   residual regression is the *deliberately-starved* GB10 0.30 pool (dominated by the inherent
   mid-pipeline `apply_boolean_mask` selection sync + cold pool-init, not removable churn — the pool is
   ~35 GB, the extra `index_seq` ~10 MB); raise `late_materialize_min_rows` above the offending split
   size on a memory-starved box to force eager there. Absolute starved-pool times are pool-warmup-order
   dependent, so don't tune off a single run.
3. **Selection-gate `kernel_compute_compressed_offsets_fsst`** — it still runs over *all* rows
   (selection-independent) and is now ~50% of late-mat GPU time for plain FSST (not just DICT_FSST as
   the old note assumed). Lower priority since GPU time isn't the e2e bottleneck.

Note on the `index_seq` over `total_rows`: not worth removing. The only sync-free alternative
(`thrust::copy_if` over a counting iterator) still needs a `total_rows`-capacity output, and the
`apply_boolean_mask` it feeds already produces `num_selected` via an unavoidable size sync — so it
saves neither a sync nor meaningful peak memory against a 35 GB pool.

The original design notes below are unchanged (the record of how the prototype was built).

---

---

## Goal

For the GPU-native DuckDB decode path (`sirius_gpu_duckdb_native_scan`), late-materialize the varchar
columns: **decode the fixed-width filter columns first, build a selection vector from the surviving
rows, then materialize the (non-filter) string columns straight into compacted form *inside the decode
kernels*** — so the decode + chars-write work for filtered-out rows is never done. Selection is applied
at the earliest point in the string pipeline (Pass-1 length computation), not as a post-decode gather.

Target workload: TPC-H @ SF100 queries that project string columns which are **not** part of the
pushed-down filter (so the filter columns are all fixed-width and the strings ride the selection).

---

## What is DONE

### 1. Kernel-level selection (the core ask) — `src/cuda/scan/gpu_decode_strings.{cu,cuh}`
- New `string_decode_selection { uint32_t const* d_sel; uint32_t num_selected; }` parameter on
  `gpu_decode_strings_column(...)` (default `{}` = decode all rows, byte-for-byte the old path).
- A **row-selection policy** templated into every per-row codec kernel (`identity_select` for the
  all-rows path, `vector_select` for late-mat). Each kernel walks its chunk in *output-position*
  space: `out_lo/out_hi` clip the chunk's global row range to its slice of `d_sel` via two device
  `lower_bound`s; `in_k(out_pos)` maps an output slot back to the chunk-relative input row. Zero
  overhead on the identity path (specialized, no indirection in the hot loop).
- Both passes (length + gather) for **all five codecs** — UNCOMPRESSED, DICTIONARY (short/long-warp),
  FSST, DICT_FSST (modes 0/1/2) — write each surviving row's length/chars straight to its compacted
  output slot. `d_lengths`/`d_offsets`/chars are all sized to `num_selected`. The chars buffer is
  sized exactly via the forced exact-total read-back (one sync) when a selection is active.
- New `kernel_gather_validity_bits`: bit-gathers the full-resolution null mask down to the
  `num_selected` output mask (one warp emits one 32-bit word via `__ballot_sync`).

### 2. Integration wiring — `src/op/scan/duckdb_native_decoder.{cpp,hpp}` + `duckdb_native_gpu_ingestible.{cpp,hpp}`
- `late_materialization_plan { std::function<unique_ptr<column>(table_view)> evaluate_mask; }` passed
  into `decode_duckdb_native_split(...)`. The decoder: decodes fixed-width cols → calls `evaluate_mask`
  → one `apply_boolean_mask` compacts `[row-index-sequence | fixed-width cols | rowid cols]` together
  (the compacted index column **is** the selection vector) → decodes varchar cols with that selection.
- `build_fixed_width_filter_ast(...)` builds the filter AST in **dense fixed-width index space**
  (projected non-rowid, non-varchar columns, in projected order) — the layout the decoder hands to
  `evaluate_mask`. Viability gating (returns `nullptr` → eager fallback) requires: ≥1 projected varchar,
  ≥1 fixed-width column, and **every** pushed-down filter references a fixed-width column (else a
  varchar/rowid filter would be silently dropped and admit rows the query must reject). Translation
  failures also fall back. Late-mat is strictly an optimization — never changes results.
- `materialize_table` builds the per-call `evaluate_mask` closure (own `gpu_expression_executor` over
  the shared read-only `_fw_filter_ast`) and returns `filter_state::ROW_FILTERED` so
  `post_filter_and_project` skips the now-redundant row filter and only projects (drops trailing
  filter-only columns). `next_split_provider` drops the post-decode filter when late-mat is active.

### 3. Config — `late_materialize_native_strings` (default **true**)
`src/include/sirius_config.hpp`, `src/sirius_config.cpp`, `src/sirius_extension.cpp`. Toggle with
`SET late_materialize_native_strings = false;` for the eager-decode-then-filter baseline.

### 4. Tests + microbench
- `test/cpp/scan/test_gpu_decode_strings_selective.cpp` (new) — for each codec/mode, asserts
  `selective_decode[i] == full_decode[d_sel[i]]` (decode-with-selection == decode-then-gather) across
  strides 1/2/5/50 + the empty-selection case. **All 11 cases pass (413,946 assertions).**
- `test/cpp/scan/strings_multiseg_synth.hpp` (new) — multi-segment TPC-H-like comment synth.
- `test/cpp/scan/bench_decode_codecs.cpp` (+5 `[!benchmark]` cases) — kernel-only selective microbench
  at 100/50/10/1% keep.
- `CMakeLists.txt` registers the new test.

**Build is clean** (release, all objects rebuilt after edits). Validated against cucascade `dfd2ff0`,
but the late-mat code uses no new cucascade APIs — it builds against the `dev` base submodule too. The
local working tree also had an unrelated cucascade submodule bump; it is **not** part of this branch.

---

## What is PROVEN (kernel microbench, 4M rows, RTX PRO 6000-class, keep% = rows surviving filter)

| Codec (string col)            | 100% keep | 50%   | 10%   | 1%     | Verdict |
|-------------------------------|-----------|-------|-------|--------|---------|
| DICTIONARY                    | 0.84x     | 1.55x | 5.79x | 13.25x | scales great |
| FSST                          | 0.94x     | 1.59x | 4.61x | 6.23x  | scales great |
| DICT_FSST mode-1 (low-card)   | ~1.0x     | 1.12x | 1.33x | 1.39x  | modest |
| DICT_FSST mode-1 (high-card)  | ~1.0x     | ~1.0x | ~1.0x | ~1.0x  | **flat** |
| DICT_FSST mode-2 (FSST_ONLY)  | 0.80x     | 1.01x | 0.99x | 1.05x  | **flat** |

Two takeaways:
1. **DICTIONARY / FSST late-mat works as intended** — up to 13x / 6x when 1% of rows survive.
2. **DICT_FSST is flat (~1.0x).** This is the optimization ceiling frontier (see below). It matters
   because DICT_FSST mode-2 / high-card mode-1 is exactly what DuckDB picks for wide high-cardinality
   text like `l_comment`, `o_comment`, `c_comment`.
3. At **100% keep** there's a slight regression (0.80–0.94x) from the selection overhead (two binary
   searches/chunk + the forced exact-total sync). Worst case = a pushed-down filter that passes
   everything. Acceptable for a prototype; see "open question" below.

---

## What is NOT done (resume here)

### A. End-to-end validation against `test_datasets/tpch_sf100_native.duckdb` ← do this first
The integration path compiles but has **never been run end-to-end**. Nothing yet proves the
`evaluate_mask` closure, the dense-fixed-width filter remap, `ROW_FILTERED` post-processing, and the
projection-drop produce correct query results on a real plan. **Correctness gate before any profiling.**

Suggested check (toggle late-mat off vs on, and vs stock DuckDB):
```bash
cd /home/kevin/sirius
B=build/release/duckdb
EXT=build/release/extension/sirius/sirius.duckdb_extension
# For each candidate query Q: run with late-mat ON vs OFF and diff results.
for sw in true false; do
  pixi run $B test_datasets/tpch_sf100_native.duckdb -c "
    LOAD '$EXT';
    SET gpu_execution=true;
    SET enable_gpu_duckdb_native_scan=true;
    SET late_materialize_native_strings=$sw;
    .read test/tpch_performance/tpch_queries/q10.sql" > /tmp/q10_$sw.out
done
diff /tmp/q10_true.out /tmp/q10_false.out && echo "MATCH"
```
Candidate queries (string projected, NOT filtered → fixed-width-only filter):
- **Q10** — projects `c_name,c_address,c_phone,c_comment,n_name`; filters `o_orderdate`,`l_returnflag`(char).
- **Q1** — projects `l_returnflag,l_linestatus` (tiny char, likely RLE/DICTIONARY); filter `l_shipdate`.
- **Q2 / Q20 / Q21** — project `s_name,s_address,s_comment` etc.; verify the filters are all fixed-width.
  (Watch out: any `LIKE`/string filter makes the col a *filter* col → late-mat correctly declines.)
Also confirm via `SIRIUS_LOG_DEBUG` that "late-materializing varchar columns" actually fires (i.e. the
gating accepted the query) — if it always declines, the win is zero regardless of kernels.

### B. SF100 nsys profiling — measure the real end-to-end win
Microbench is kernel-only. Profile the whole scan to see late-mat's share and confirm the speedup
survives the apply_boolean_mask + executor overhead. Harness: `test/tpch_performance/` (see its
`run.md` / `CLAUDE.md`; `profile_tpch_nsys.sh`, `nsys_report.sh`). Use the `profile-analyzer` /
`optimization-advisor` skills on the resulting nsys reports.

### C. Break the DICT_FSST ceiling (the actual "optimize until ceiling" work)
DICT_FSST stays ~1.0x because its **selection-independent prep passes dominate and run over all rows /
all dict entries**, regardless of the selection:
- `kernel_compute_compressed_offsets_fsst` — per-segment compressed-cumsum over **every** row
  (`d_comp_offsets` deliberately stays in full segment-row space; FSST addressing needs every row's
  compressed cumulative length to find a row's compressed bytes).
- `kernel_predecode_dict_fsst` / decoded-offsets — decode **all** dictionary entries (mode-1) even if
  the selection touches few of them.
Ideas to explore (in rough order): (1) for mode-2/FSST-only, the per-row decompress is already
selection-gated in Pass-2 but Pass-1 length still needs the comp cumsum — see whether the cumsum can
be computed lazily only for selected rows' segments; (2) for mode-1, predecode only the dict entries
referenced by surviving rows (build a referenced-entry set from `d_sel` first); (3) accept that
DICT_FSST is prep-bound and document late-mat as a DICTIONARY/FSST optimization. Measure each against
the microbench before/after.

### D. Open question — 100%-keep regression
Gating engages on "filter present", not on selectivity (unknowable pre-decode). When the filter passes
~all rows, late-mat is a slight net loss. Options: leave as-is (filters that select everything are
rare), or skip the selection compaction when `num_selected` is close to `total_rows` (needs the count,
which costs a sync). Decide after the SF100 numbers.

---

## Quick resume

```bash
git fetch origin && git checkout kk/late-mat-native-strings
git submodule update --init --recursive
pixi run make                                 # release build
# kernel correctness + microbench (fast, no dataset needed):
pixi run build/release/extension/sirius/test/cpp/sirius_unittest "[selective]"
# then: section A (e2e correctness on tpch_sf100_native.duckdb) → B (profile) → C (optimize)
```

## Key files
- `src/cuda/scan/gpu_decode_strings.cu` / `.cuh` — selection policy + per-codec kernels (the core).
- `src/op/scan/duckdb_native_decoder.cpp` — `decode_duckdb_native_split` late-mat branch.
- `src/op/scan/duckdb_native_gpu_ingestible.cpp` — `build_fixed_width_filter_ast`, `materialize_table`.
- `test/cpp/scan/test_gpu_decode_strings_selective.cpp` — correctness oracle.
- `test/cpp/scan/bench_decode_codecs.cpp` (search `late-mat selective`) — microbench.
