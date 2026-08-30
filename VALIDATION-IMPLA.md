# VALIDATION — IMPL-A (IO/copy concurrency defaults, PROP-02/08)

Branch: `impl-a-reactor-defaults` (worktree `/localhome/local-kkristensen/Code/sirius-impl-a`,
based on 1ad67e7f). Build status: `pixi run make` clean (release). No GPU binary was executed by
the implementer.

## What changed

- `src/include/scan_manager/config.hpp`
  - New pure, constexpr derivation `scan_manager::derive_uring_n_reactors(int)` =
    `min(max(1, pipeline_num_threads), 8)`; cap constant `max_derived_uring_n_reactors = 8`.
    The `>= 1` clamp is the A-2 hang guard (0 reactors = empty reactor pool at
    `datasource_factory.cpp:127`).
  - `default_uring_n_reactors` is now `derive_uring_n_reactors(exec::default_gpu_pipeline_num_threads)`
    (= 4 with the stock pipeline default), so default-constructed configs are self-consistent
    without the finalize.
  - Budget arithmetic parameterized as `scan_manager_num_threads_for(pipeline_threads, reactors)`
    (A-3: the old constexpr `reserved` could not hold a runtime-derived count);
    `default_scan_manager_num_threads()` delegates to it with the defaults. Floor of 4 unchanged.
- `src/sirius_config.cpp`
  - A-1 sentinel: the scan_manager `from_yaml` now records `has_value("num_threads")` /
    `has_value("uring_n_reactors")` into a `scan_manager_explicit_keys` struct, so an explicit
    yaml `uring_n_reactors: 1` (equal to the old default) survives.
  - Post-parse finalize `finalize_scan_manager_concurrency(...)` runs after the whole
    `sirius.executor` block is parsed (scan_manager parses before pipeline): derives
    `uring_n_reactors` from the effective pipeline thread count unless explicitly set, then
    recomputes the scan-manager pool budget unless `scan_manager.num_threads` was explicit.
- Tests: `test/cpp/config/test_config.cpp` — two new TEST_CASEs, tag `[uring_reactors]`
  (also tagged `[config_opt][scan_manager]`).
- Docs: `docs/super-sirius/configuration.md` (scan-manager table + pairing/bounce-buffer/pool-margin
  paragraph, pipeline cross-reference, example config), `test/tpch_performance/run.md`
  (GB300 example now pairs pipeline threads 8 with derived reactors, `initial_number_pools` 785 -> 60,
  plus the two sizing rules per ANALYSIS.md Addendum 2).

## Intentional behavior deltas (not flagged as silent)

- Default `uring_n_reactors`: 1 -> 4 (stock config, pipeline default 4); scales to 8 when
  `executor.pipeline.num_threads >= 8`.
- Default scan-manager pool: shrinks by the extra reactors AND now subtracts the *effective*
  pipeline thread count (not the compile-time default) when `scan_manager.num_threads` is unset.
  Rationale: the budget's contract is "every core left after the other pools"; deriving reactors
  from an overridden pipeline count while budgeting against the default pipeline count would
  oversubscribe. Example (72-core GB300): defaults 72-(1+1+4+4)=62 was 72-(1+1+4+1)=65; with
  pipeline 8: 72-(1+1+8+8)=54.
- Explicit yaml `uring_n_reactors` always wins, including values of 1 (single-reactor reachable)
  and values above the cap of 8.
- Byte-identity: preserved. This change alters IO timing/parallelism only; split composition is
  fixed by the metadata-fed coalescer before reactors touch anything (per CONTRACTS_REVIEW
  cross-contract note). No row/batch-order change.
- No new syncs; no GPU-path code touched.

## What the orchestrator must run

1. **Focused unit tests** (GPU harness init happens regardless; tests themselves are CPU-only):

   ```bash
   pixi run build/release/extension/sirius/test/cpp/sirius_unittest "[uring_reactors]"
   ```

   Expected: 2 test cases, all assertions pass. Coverage: derivation formula incl. cap and >= 1
   clamp (compile-time STATIC_REQUIREs), finalize from effective pipeline count (6 -> 6 reactors),
   explicit `uring_n_reactors: 1` surviving pipeline 8 (the A-1 clobber case), explicit
   `num_threads` surviving while reactors still derive, and derived defaults with no executor
   block.

2. **Config regression sweep** (same binary):

   ```bash
   pixi run build/release/extension/sirius/test/cpp/sirius_unittest "[config_opt]"
   ```

   Expected: all pass (parse-shape untouched for every other key; `reject_unknown` behavior
   unchanged).

3. **Path-fired evidence (runtime)**: load the extension with `sirius_log_level = debug` and no
   `uring_n_reactors` in yaml. The scan manager init logs

   ```
   [sirius_scan_manager] sirius_datasource enabled (uring_ioctx n_reactors=N)
   ```

   Expected N = min(pipeline num_threads, 8): 4 with a default config, 8 with
   `executor.pipeline.num_threads: 8`, and 1 when yaml sets `uring_n_reactors: 1` explicitly
   (sentinel proof). Startup must not hang (A-2) and per-reactor init adds a 64 x 1 MiB pinned
   bounce allocation each -- watch startup pinned footprint at high counts.

4. **SQL correctness**: any standard suite block, e.g.

   ```bash
   pixi run build/release/test/unittest --test-dir . test/sql/tpch-sirius.test
   ```

   Expected: byte-identical results vs base (this change cannot legally alter values or order).

5. **Perf A/B (R1, the predicted win)**: hunt harness R1 chain, this branch vs 1ad67e7f, default
   configs on both sides (that is the point: the win becomes default-on). Prediction: R1 figure of
   merit 21.85 -> ~5.4 s fast-mode; q19 hot ~15 s -> ~3 s. Per ANALYSIS.md Addendum 2, run >= 4
   blocks/side with the iostat rider labeling each block's mode (hot-iteration disk GB) and report
   the fast-mode figure + mode frequency, not a blind median; with the canonical 421 GB pre-fault config
   the page-cache margin is bimodal, so pair the A/B with the pools-60-class sizing from the
   updated `test/tpch_performance/run.md` for deterministic blocks.

## Adjacent observations (not implemented, per contract)

- The kvikio fallback path (`use_sirius_datasource: false`) ignores `uring_n_reactors` entirely;
  no derivation needed there.
- `rest_n_reactors` (S3 path) keeps its constant default of 2; the same derivation may apply but
  was not in scope and is unmeasured.
