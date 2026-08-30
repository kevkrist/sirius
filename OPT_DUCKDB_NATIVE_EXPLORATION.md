# DuckDB-native SCAN optimization exploration — reproduction guide

Branch `opt/duckdb-native-exploration`, based on dev `1ad67e7f` (2026-08-27).
Campaign: 2026-08-28..30 on pmgb300ws (GB300, aarch64, 486 GB LPDDR + 249.8 GiB FB, driver
595-open). This document is self-contained: environment, dataset, measurement protocol, every
optimization with its mechanism and measured effect, and the pitfalls that cost us measurement
validity. Everything below was verified byte-identical to baseline with zero downgrade lines.

## 1. What is on this branch (commit stack)

Applied directly (validated together as an integration; per-item A/Bs below):

| Commits | Item | Summary |
|---|---|---|
| 3 cherry-picks (`perf(transparent)…`, `perf(scan): defer…`, `fix(transparent)…`) | **PR #1548 rebase** | Build the Sirius physical plan once per query; skip the per-row-group metadata walk for pin-served scans. One mechanical conflict vs dev resolved in `insert_pinned_entry*` (kept `publish_late_mat_handle` + added epoch bump after it). |
| `cf2c1d54` | **P11 — decode descriptor packing** | All of a split's codec-run descriptors (bitpacking/RLE/ALP/dict/string maps) pack into one pinned blob → ONE H2D per split, replacing hundreds of pageable `cudaMemcpyAsync`s (~4.5 ms driver staging each; 2.2 s/q19-iter) and the per-varchar unconditional stream syncs. `batch_null_count` skipped when a split stages no validity runs. DICT_FSST keeps its inherent round trips. New files `src/{include/,}cuda/scan/decode_descriptor_arena.*`. |
| `d3e34eb0` | **P2 — read→H2D→decode overlap** | `submit_and_await` no longer blocks on the whole split's read then serially H2Ds then blanket-syncs: reads issue as ≥512 MiB sub-batches; each completed sub-batch's H2D enqueues immediately; pinned staging released via CUDA events (thread-local deferred release drained before the pool's blocking reservation to avoid self-deadlock). Error path joins ALL futures + syncs issued H2Ds before unwind (that blanket sync was load-bearing for exception safety). |
| `7854511a` | **Teardown fix for P2/P11** | TLS destructors (pinned slab pool, deferred release) made `noexcept`; on shutdown they swallow CUDA errors and deliberately leak (cucascade releases in a TLS dtor after manager teardown threw → `__cxa_call_terminate` at process exit on gpu-pin runs). Follow-up documented: a SiriusContext-owned registry would free mid-lifetime thread-exit leaks too. |
| `08541e13` | **P8 — dynamic-filter membership fold** | When a native scan carries a static pushed filter AND a published DF membership snapshot, membership is AND-ed into the static mask (reusing the existing probe closures — NULL/semi-join semantics identical) so the split gathers ONCE at final selectivity instead of static-gather → DF-op gather → residual filter. Unpublished snapshot ⇒ old path bit-for-bit; q1/q6 (no DF) ⇒ one pointer check. In `post_filter_and_project` (`duckdb_native_gpu_ingestible.cpp`). |
| `78dc0b7a` + log commit | **impl-a — uring reactor default derivation** | `default_uring_n_reactors` = `min(pipeline_threads, 8)` instead of constant 1, via post-parse finalize with explicit-yaml sentinels (scan_manager parses before pipeline; explicit `uring_n_reactors: 1` survives; derived count clamps ≥1). Effective count logged at INFO. Docs updated. |

Carried as patches in `opt-patches/` (apply with `git apply`; reasons below):

| Patch | Item | Why not applied |
|---|---|---|
| `impl-b-cucascade-sm-copy.patch` | **SM gather-copy H2D upload engine** (cucascade, apply inside the submodule at `1b0e7b6c`) | Lives in the cucascade submodule — no public remote holds the commit, so the branch keeps the base submodule pointer to stay checkout-able. Value is host-tier pin regimes only (−0.35 s / −11% R2 there): 343 GB/s SM copy vs 184 GB/s `cudaMemcpyBatchAsync` + removes 1.12 s/245 GB host-thread blocking at 1 MiB pin blocks. Grid capped at 0.25×4×SMs (full grid contended with concurrent compute; sweep-chosen). Env: `CUCASCADE_CONVERT_H2D_USE_SM_COPY` (default on), `CUCASCADE_SM_COPY_MAX_BLOCK_FRACTION` (default 0.25). Inert under gpu-tier pinning / 64 MiB blocks. |
| `impl-c-constant-batch.patch` | **Batched CONSTANT-codec decode** | Conflicts with P11/P2's decoder restructure (both rewrite the staging/dispatch window) — needs a rebase onto this branch's decoder. Mechanism: clustered data makes l_shipdate 95% CONSTANT (46k segments); the old path did one 8 B pageable copy + one kernel launch PER SEGMENT (~25k each per q1-iter, 145 ms host serialization → 24 µs batched). Memory/hygiene value (Kevin's framing); end-to-end shadowed by the read path today. |
| `impl-d-alltrue-mask.patch` | **All-true/all-false mask fast paths** | Conflicts with P8 (same filter/gather block). Mechanism: 27/28 clustered q1 splits pass their filter 100%, and `apply_boolean_mask` deep-copies the whole table anyway — the patch counts survivors via a free positions-compaction readback, MOVES columns when all pass (owned tables only; view-backed would add a sync), emits empty batches for all-false. Removes ~128 GB/iter HBM traffic + 10.7 GB/split transient; FOM-flat in both measured regimes. Adds additive `expression_evaluator::survivor_indices()` + `owning_table_view::is_no_alloc_materializable()`. |

**Evaluated and rejected, with evidence (do not re-implement without new data):**
reservation right-sizing (accurate bounds admitted more tasks into a 97%-pinned GPU → partial-
reservation thrash, q1 gpu-pin +0.48 s; branch `proto-b-reservations` @4b154402 preserved — revisit
stacked on P8); pinned-pool shrink for R1 (free RAM was never the constraint); dictionary string
carrier for pins (device reconstruction eats the byte savings, 4 vs 30 ms/chunk threshold);
`memory_prefetcher` (measured 0 bytes moved — it is enabled in the nightly GB300 profile for
nothing); O_DIRECT reads (hot iterations are page-cache-served); ast_jit removal (default strategy
is WORSE: +0.12 s); disabling `SIRIUS_EXP_FUSED_SCAN_FILTER`/`LATE_MAT` (2.6× WORSE on from-disk —
these flags are load-bearing).

## 2. Measured results

FOM = Σ over {q1,q6,q14,q19} of median hot-iteration time, TPC-H SF1000 clustered duckdb-native.
Reference regime = the nightly `benchmarks.yaml` GB300 profile (§4) — NOT the run.md doc example.

| Regime | Baseline | This branch (+`uring_n_reactors: 8`) | Δ |
|---|---|---|---|
| From-disk (`--pin none`) | 7.70 s | **~5.0 s** | **−35%** |
| Nightly-pinned (`--pin gpu --pin-after-iteration 1`) | 1.52 s | **1.13 s** | **−26%** |

Attribution (each individually A/B'd, interleaved): reactors 4→8 config −1.6 s R1; P11 ≈ −0.3;
P2 ≈ −0.5; #1548 −0.34 R2 (and −0.30 R1); P8 −0.09 R2 (q19 −15%). Under the older documented
config (1 reactor, 1 MiB pin blocks, host-tier pins) the same branch measures far larger R1 deltas
(21.85 → ~5.4 s) because the reactor default change bites — that regime is where impl-a/impl-b
carry their value. Host-tier transfer test: 2.58 vs 3.19 (−19%).

## 3. Environment and dataset

- GB300/Grace-Blackwell prereqs: open-kernel driver; `echo online_movable | sudo tee
  /sys/devices/system/memory/auto_online_blocks` + module reload; `nvidia-smi -pm 1`. Without
  both, CUDA init fails or large pools OOM while nvidia-smi looks fine.
- Build: `git submodule update --init --recursive && pixi run make` (fresh worktrees need both).
- Dataset (284 GB, ~2.5 h; needs ~1.2 TB free during build):
  `cd test/tpch_performance && pixi run bash generate_tpch_data.sh 1000 --format duckdb --cluster`
  → `test_datasets/tpch_sf1000_sorted.duckdb` (clustered `lineitem:l_shipdate,orders:o_orderdate`;
  clustering makes row-group pruning live and l_shipdate 95% CONSTANT-codec — several
  optimizations' magnitudes depend on it). Ours: sha256 `896d1900…c50c`.

## 4. The reference configuration (decides what transfers!)

Transcribe from `benchmarks.yaml` `gpu_profiles.GB300` + `sf_memory.1000` — verbatim, including:
`uring_n_reactors: 4` (raise to 8 = the config half of the R1 win), `pipeline.num_threads: 8`,
`scan_manager.num_threads: 18`, `memory_prefetcher.enable: true` (no-op, but keep for fidelity),
`scan_task_batch_size: 8GB`, `hash_partition_bytes/max_build_hash_table_bytes: 32GB`,
`usage_limit_fraction: 0.95`, host pool `392 × 8 × 64 MiB` (≈196 GB initial, 471.2 GB cap),
disk spill at `/tmp/sirius_disk_memory`, and env on every run:
```
SIRIUS_EXP_FUSED_SCAN_FILTER=1 SIRIUS_EXP_LATE_MAT=1
SIRIUS_EXP_LATE_MAT_PIN_UNIQUE_COLS=c_custkey,n_name,n_nationkey
SIRIUS_PRE_SQL="SET expression_evaluator_strategy = 'ast_jit';"
```
The campaign's costliest lesson: our first full pass measured against the run.md doc example
(1 reactor, 1 MiB blocks, host pins, no env flags) and several "wins" did not transfer. Every
finding here carries an implicit dependency vector on the values above; if your machine's profile
differs (see GB200/GB10/RTX profiles — which today set NO reactor count and so run 1), expect the
impl-a default to be the dominant effect.

## 5. Measurement protocol (what makes numbers trustworthy on this hardware)

1. Harness: `pixi run python test/tpch_performance/performance_test.py --input <db> --data-source
   duckdb --engine gpu --config <yaml> --mode grouped --iterations 5 --queries 1,6,14,19
   --pin {none | gpu --pin-after-iteration 1} --query-timeout 1800`. Hot = iters 1–4 (from-disk)
   / 2–4 (pinned). Correctness: same command with `--engine both --validation`.
2. **Interleave candidate/baseline blocks in one chain** and compare only within-chain. Never use
   `--pin host` with `--mode sequential` at SF1000 (union-pins all 22 queries' columns → host cap OOM).
3. **Serialize ALL GPU work** (one flock lease): `sirius_unittest` initializes GPU pools even
   tag-filtered. `nvidia-smi memory.used` is NOT an idle signal on coherent-memory machines (page
   cache reports as used); gate on compute processes.
4. **Quiescent host**: co-located CPU load (IDE indexers, agent jobs, trace mining) collapses the
   reactor read lanes (device reads 6.7 → 2.4 GB/s, same volume) — gate blocks on loadavg and run
   an `iostat -x 1` rider per block; label each block by hot-iteration disk GB (≈ one cold pass
   ≈ 97 GB is healthy; 2× = cache-retention failure; low-rate = contention collapse). Discard
   labeled-bad blocks; two per ~40 in our campaign even with the gate.
5. Old-config warning: at a 421 GB pinned pre-fault (785×512×1 MiB), from-disk hot behavior is
   BIMODAL (cache-served vs disk-bound) because node-0 free RAM ≈ working set; the nightly's
   196 GB pre-fault does not exhibit this.
6. Two pre-existing test flakes (fail identically at base `1ad67e7f`): `test_owning_table_view.cpp`
   "release awaits the copy…" OOMs under combined-tag runs; `bench_decode_codecs.cpp:502` OOMs in
   a 307-case union process. Neither is a regression signal.

## 6. Validation on this branch

```
pixi run build/release/extension/sirius/test/cpp/sirius_unittest \
  "[uring_reactors],[config_opt],[scan][decode],[scan][decode][arena],[scan][decode][overlap],[duckdb_native_df_fold],[scan][duckdb_native_empty_split],[scan][duckdb_native_host_backed],[dynamic_filter][scan_merge]"
```
plus `--engine both --validation` on {1,6,14,19}, both pin modes, nightly env. Expect: all pass,
byte-identical results, zero `[downgrade]` lines, INFO line `uring_ioctx n_reactors=8` (derived),
DF-fold debug lines on q19 pinned runs, and rc=0 process exits (the teardown fix's regression bar).

## 7. Full campaign record

The complete artifact tree (contracts, code maps, analyst reports, per-run manifests/iostat
riders, nsys traces, microbenchmark sources incl. the 343 GB/s gather-copy and CONSTANT-broadcast
benches, and the V1 host-tier-regime results) lives outside the repo at
`~/sirius-hunts/scan-native-ddb-20260828/` on pmgb300ws — start with `REPORT-V2.md` and
`FEATURES.md` there. Per-feature validation notes are committed here as `VALIDATION-*.md`.
