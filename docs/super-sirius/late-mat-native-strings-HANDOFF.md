# Late materialization for the DuckDB-native string decode — handoff

**Branch:** `kk/late-mat-native-strings`  ·  **Base:** `dev` @ `f1eb9e02`  ·  **Status:** prototype
complete + kernel-level validated; end-to-end validation and SF100 profiling not yet run.

This note is the pick-up-where-I-left-off for the late-materialization prototype. It records what is
done, what is proven, what is *not* yet done, and the exact commands to resume on another machine.

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
