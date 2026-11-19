# Tooling scout: benchmarking / profiling the DF stack (#2569, #2570)

Read-only scout, 2026-11-18. Nothing was run on GPU. Paths: `R=/opt/dlami/nvme/dfperf`, repo `$R/repo`
(pr2570 tip `ae939824`; pr2569 tip `a2d6bd5a`; merge-base `c4fa3638`; 16 commits in stack).
State at scout time: `setup.sh` still cloning submodules; **no build yet, `$R/data` empty**. Every command
below is unverified until the build + datagen finish.

## 0. Machine / environment facts

- 4x RTX PRO 6000 Blackwell 97,887 MiB, 96 cores, 999 GB RAM. P2P only between GPU 0-1 and 2-3
  (S5B handoff S0) -> cross-pair Bloom replicas go through HOST staging (user says: ignore).
- nsys `2026.3.2.476` (/usr/local/bin/nsys); ncu `2026.1.0.0` (/usr/local/cuda/bin/ncu).
- Root disk: 11 GB free. NVMe 6.6 TB free (instance storage: wiped on instance stop).
- Config `/home/ubuntu/sirius/specific_config.yaml` (never alter): 4 GPUs, pipeline 4 thr, task_creator 4,
  `scan_task_batch_size 4GB`, `hash_partition_bytes 1GB`, `concat_batch_bytes 1GB`,
  `max_build_hash_table_bytes 8GB`, `max_broadcast_join_size 3GB`, host 600 GB,
  `enable_pin_table_compression: true` (plans `/home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000`,
  exists: lineitem.txt, orders.txt, customer_disabled.txt), **`telemetry.enable_quent: true`,
  `output_directory: /home/ubuntu/sirius/results/telemetry`** (no `enable_nvtx`).

### Root-disk hazards (things that write outside NVMe by default)
| Writer | Default location | Redirect |
|---|---|---|
| Quent NDJSON (always on with this config) | `/home/ubuntu/sirius/results/telemetry/<session-uuid>/` (root; already 2.1 GB / 245 sessions; ~10 MB per small session, grows with task count) | Only via config -> cannot. Watch `du -sh`; ask user before moving/deleting old sessions or using a config copy |
| `performance_test.py` output | `test/tpch_performance/output/` in repo | `--output $R/runs` (always pass) |
| Sirius logs | `SIRIUS_LOG_DIR` (performance_test.py sets it inside benchmark dir) | fine once `--output` is on NVMe; for CLI runs `export SIRIUS_LOG_DIR=...` or `SET sirius_log_dir` |
| Simpatico JIT kernel cache (compressed pins) | `$XDG_CACHE_HOME` or `~/.cache` (src/compression/simpatico_codegen/src/jit/kernel_cache.cpp:47-54) | `export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit` |
| nsys temp + shell `mktemp` | `/tmp` | `export TMPDIR=$R/tmp` |
| pixi / sccache | per setup.sh | `PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache` |

Common env block for everything below:
```bash
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR $R/runs
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
export DATA=$R/data/tpch_parquet_sf1000
cd $R/repo
```
Rule: one Sirius/GPU process at a time (timing, nsys, unit tests, microbench all contend).

## 1. Bloom cap: the handoff's SET no longer exists

- S5B handoff (`/home/ubuntu/sirius/S5B_MGPU_HANDOFF.md`) required `SET max_dynamic_filter_bloom_bytes_per_gpu = 10264195891`
  for both builds, because the old tip enforced a 256 MiB cap the old base ignored (q3: 294 MB Bloom on
  `orders` 147M rows refused -> 1.14x on 1 GPU).
- At merge-base, pr2569 and pr2570 the knob is **removed** (YAML key throws: src/sirius_config.cpp:680-684).
  Budget is engine-derived: `max(4 * scan_task_batch_size, max_build_hash_table_bytes)`
  (src/sirius_config.hpp:272-275) = max(16 GB, 8 GB) = **16 GB/GPU** with this config. So q3's Bloom fits;
  no SET needed or possible. Refusals still log at info:
  `[plan_construction] Bloom filters refused: per-GPU cap, keys=, required_bytes=, cap=` (src/op/dynamic_filter/construction_plan.cpp:231).
- `default_max_dynamic_filter_bloom_bytes_per_gpu = 4 * 800 MiB` (src/op/dynamic_filter/config.hpp:35) is only
  the publish-plan default used by tests; pr2570 touches this file.
- DF SQL settings that exist: `enable_dynamic_filter`, `enable_dynamic_zone_map_filter`,
  `dynamic_filter_domain_coverage_threshold`, `dynamic_filter_keep_threshold` (src/sirius_extension.cpp:3636-3955);
  YAML also `dynamic_filter_inlist_max_l2_fraction` (src/sirius_config.cpp:689). `build_fold_policy` (PR3) is NOT in this stack.
  Useful A/B knob: `SET enable_dynamic_filter = false` (price of all DF work + benefit).

## 2. (a) Full TPC-H SF1000 timing run (unprofiled)

Canonical runner = `test/tpch_performance/performance_test.py` (in-process Python duckdb; `benchmark` skill).
No `--profile`/`--mode` => config used exactly as given, grouped order (iterations back-to-back per query),
pin per query. NOTE: the script issues `SET pin_table_compression = false` unless `--pin-compression` is
passed (performance_test.py:800-810), which would override the config's `enable_pin_table_compression: true`
=> pass `--pin-compression --compression-plan-dir` to match the config.

```bash
pixi run python test/tpch_performance/performance_test.py \
  --input $DATA --scale-factor 1000 --data-source parquet --engine gpu \
  --config $SIRIUS_CONFIG_FILE --iterations 4 \
  --pin host --pin-compression \
  --compression-plan-dir /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 \
  --output $R/runs --name tip_a
# subset: add  --queries 3,5,7-10
```
Output: `$R/runs/tpch_<ts>_default_gpu_iter4_tip_a/{csv/runtimes.csv (engine,query,iteration,runtime_s), config.yml,
metadata.json, log_dir/, sirius/q<N>/{result.txt,sirius.log}}`.
Open question for the user: which regime did they validate "no regression" in (host-pinned compressed vs
parquet-from-disk + scan cache)? S5B kit used host pin for all tables, 1 warm + 5 hot passes, median of hot.

Alternative shell runner (DuckDB CLI, `.timer`, single session, cold + warm back-to-back):
```bash
SIRIUS_PIN_TIER=host pixi run bash test/tpch_performance/run_tpch_parquet.sh \
  --parquet-dir $DATA --iterations 4 --timeout 0 --pinning-mode pinned-hot sirius 1000 $(seq 1 22)
```
(no pin-compression flag here: the config value applies; `OUTPUT_DIR=...` to keep results off cwd; temp SQL in $TMPDIR.)

Extra SQL after LOAD for any performance_test.py run: `SIRIUS_PRE_SQL="SET sirius_log_level='debug';"` (performance_test.py:811-814,1160).

Fallback/error scan after every run (a result file is not proof of GPU execution):
```bash
rg -l "Transparent execution fallback|fallback to DuckDB|falling back to DuckDB" <bench>/sirius/q*/sirius.log
grep -lE "Error in Sirius|rmm::bad_alloc|out of memory|illegal memory access" <bench>/sirius/q*/sirius.log
grep -c "reschedule (retry" <bench>/sirius/q*/sirius.log     # OOM retries
grep -c "assigned pinned entry" <bench>/sirius/q*/sirius.log # pin actually used
```

### Measurement protocol / caveats
- Iteration 0 = first run after pin (plan + JIT warm, simpatico kernel JIT); use iterations >= 1; median of >= 3 hot.
- Variance: S5B P1 saw A/A geomean noise ~0.3-1% but per-query noise must be measured (A/A pair, e.g. B,C,C,B blocks);
  a delta inside A/A noise is not a win. Fresh process per label; LOAD right after a previous process may fail
  (driver still releasing the pool) -> retry.
- Quent is ON in every run with this config (both base and candidate) -> small constant overhead; compare like with like.
- nsys timings are inflated: never use for verdicts.
- Builds to compare: base = merge-base `c4fa3638`, pr2569, pr2570, + candidates. Use separate worktrees/build dirs on NVMe
  (`git worktree` needs `git submodule update --init --recursive`), never stash a shared tree.
- Record GPU clocks/temps (`nvidia-smi --query-gpu=clocks.sm,temperature.gpu,clocks_throttle_reasons.active --format=csv`).

## 3. (b) Quent telemetry

- Enabled purely by YAML (`telemetry.enable_quent`; src/sirius_config.cpp:725-741, src/telemetry/telemetry_context.cpp:41-47).
  One session dir per process (per LOAD): `<output_directory>/<uuid>/{Query,Operator,Plan,Task,DataBatch,Port,Channel,
  ExecutorThread,TaskManagerLoopThread,TaskQueue,GpuDevice,MemorySpace,Engine,Worker,ThreadGroup,QueryGroup}/*.ndjson + model.qmi`.
  So **every run in section 2 already produces telemetry** in `/home/ubuntu/sirius/results/telemetry/` (newest dir = `ls -t | head -1`).
- Labels: `CALL sirius_set_query_label('q3_iter2');` consumed by the next Sirius query; unlabeled = `unnamed_query`.
  performance_test.py does not label -> map by order/timestamps of `Query` Init/Executing/Finished events, or use the helper:
```bash
pixi run -- ./test/tpch_performance/run_tpch_parquet_and_generate_telemetry.sh \
  --config $SIRIUS_CONFIG_FILE --iterations 4 --parquet-dir $DATA --note tip 1000        # all 22; append "3 5 9" for a subset
```
  Helper caveat: no pinning (reads parquet; later iterations served by scan cache) -> different regime than `--pin host`.
  For labeled + pinned telemetry write a CLI script by hand:
```bash
{ # views
  for t in customer lineitem nation orders part partsupp region supplier; do
    echo "CREATE VIEW $t AS SELECT * FROM read_parquet('$DATA/$t/*.parquet');"; done   # adjust to actual layout (t.parquet | t_*.parquet | t/*.parquet); list must equal the pin glob expansion
  SIRIUS_PIN_TIER=host python3 test/tpch_performance/tpch_pin_columns.py pin-all $DATA
  echo ".timer on"
  for q in $(seq 1 22); do for i in 1 2 3 4; do
    echo "CALL sirius_set_query_label('tip_q${q}_iter${i}');"; cat test/tpch_performance/tpch_queries/orig/q$q.sql; echo; done; done
} > $R/tmp/tel.sql
SIRIUS_DISABLE=0 sirius-duckdb/build/release/duckdb -unsigned -bail \
  -cmd "LOAD 'sirius-duckdb/build/release/extension/sirius/sirius.duckdb_extension';" -f $R/tmp/tel.sql
```
  (Q11's fraction is scale-dependent: orig/q11.sql vs queries.py rendering - check before comparing results.)
- Analysis:
  - UI: `pixi run quent /home/ubuntu/sirius/results/telemetry` -> http://localhost:8080 (cargo builds `sirius-telemetry-server`;
    set `CARGO_HOME`/`CARGO_TARGET_DIR` on NVMe). Operators tab = per-operator/pipeline duration table.
  - Offline: `pixi run bash -c "cd rust && cargo run -p sirius-telemetry-analyzer --example print_resource_tree -- <dir>/<uuid>"` (resource tree only).
  - Direct NDJSON (jq / python / duckdb `read_ndjson`): `Operator` Declaration rows give pipeline uuid -> label, e.g.
    `"PARTITION(13)"`, `"GPU_SCAN(4) -> DYNAMIC_FILTER(5)"`, `"HASH_JOIN(15) -> ..."`; `Task` rows carry the FSM
    (Created/Queued/Routing/Reserving/Downgrading/Preparing/Computing{current_operator_id,input_bytes,peak_allocated_bytes,
    executor_thread,reservation}/Finalizing{success}; rust/crates/telemetry/model/TASK_FSM.md). Operator wall time =
    gap between consecutive `Computing` events of a task (one per operator) up to `Finalizing`.
  - DF relevance: construction now runs inside **PARTITION** tasks (build side), so PARTITION Computing time base-vs-tip
    is the direct cost signal; CONCAT/HASH_JOIN time should drop correspondingly; probe-side `DYNAMIC_FILTER` time and
    scan output rows show benefit; Reserving/Downgrading time shows scratch-reservation side effects.
  - No DF-specific Quent events exist; with `enable_nvtx: true` Quent would also capture `dynfilter::*` NVTX ranges,
    but that needs a config change (not allowed) -> use nsys for sub-operator DF attribution.

## 4. (c) nsys for one query + attributing time to DF code

```bash
pixi run python test/tpch_performance/performance_test.py --precmd nsys \
  --input $DATA --scale-factor 1000 --engine gpu --config $SIRIUS_CONFIG_FILE \
  --iterations 3 --queries 3 --query-timeout 3600 \
  --pin host --pin-compression \
  --compression-plan-dir /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 \
  --output $R/runs --name nsys_tip_q3
```
- One nsys-wrapped DuckDB CLI per query: `nsys profile --trace=cuda,nvtx --sample=none --cudabacktrace=none
  --capture-range=cudaProfilerApi --capture-range-end=stop --export=sqlite` (performance_test.py:1295-1345); SQL brackets all
  iterations with `CALL profiler_start()/profiler_stop()` (pin + LOAD excluded). Default `--query-timeout 90` is far too
  small for SF1000 pin. Output `<bench>/sirius/q3/{nsys.nsys-rep,nsys.sqlite,nsys.sql,timings.csv,log_dir/}`.
- `nsys_report.sh --sf 1000 --parquet-dir $DATA --output-dir $R/reports 3` wraps the same but passes **no --pin**; prefer the
  direct command, then analyze: `bash test/tpch_performance/nsys_analyze.sh <bench>/sirius/q3/nsys.sqlite`
  (operator attribution, kernels, memcpy, sync, cudaMalloc during query); `nsys_compare.sh <base_report> <cur_report>`;
  `nsys_hotspots.sh`. Skills: `profile-analyzer` (report), `optimization-advisor` (hotspot -> source).
- Under nsys NVTX goes to nsys regardless of `enable_nvtx` (nsys sets `NVTX_INJECTION64_PATH`; src/telemetry/nvtx_injection.cpp:77).
  Sirius ranges are in their own NVTX domain (`SIRIUS_NVTX_DOMAIN_NAME`; src/telemetry/nvtx.hpp).
- **DF NVTX ranges available (all in Sirius domain):**
  | Range / mark | Where | Meaning |
  |---|---|---|
  | `sirius_physical_partition::execute`, `::sink` | src/op/sirius_physical_partition.cpp:129,253 | enclosing build PARTITION task |
  | `dynfilter::construction::begin` | src/op/dynamic_filter/dynamic_filter_publisher.cpp:946 | construction start (plan, leases) |
  | `dynfilter::accum::contribute` / `::finish` | src/cuda/sirius_dynamic_bloom_filter.cu:1084,1137 | per-batch Bloom insert / reduce+replicate |
  | `dynfilter::accum::late_wait` | sirius_dynamic_bloom_filter.cu:873 | host wait |
  | `dynfilter::bounds::contribute` / `::finish` | src/cuda/bounds_accumulator.cu:340,402 | zone-map/min-max accumulation |
  | `dynfilter::stage::contribute` / `::gather` | src/cuda/exact_key_stager.cu:150,248 | exact-key (IN-list) staging |
  | `dynfilter::replicate_devices` | dynamic_filter_publisher.cpp:650 | replica copies to other GPUs |
  | marks `dynfilter::accum::final_commit`, `dynfilter::accum::visible` | dynamic_filter_publisher.cpp:1159,713 | publication latency = visible - final_commit |
  | `sirius_physical_build_filter::execute` | src/op/dynamic_filter/sirius_physical_build_filter.cpp:71 | BUILD_FILTER operator |
  | `dynfilter::apply_output`, `dynfilter::try_fused_membership_mask` | src/op/scan/dynamic_filter_merge.cpp:1122, src/cuda/dynamic_filter_mask_ops.cu:154 | probe-side application |
  Gap: no single range wraps `build_filter_construction::contribute/finish` or certification/inventory; add scoped ranges in a
  prototype if needed (cheap; note which PR they would belong to).
- SQLite attribution sketch (verify with `.schema`; nsys 2026.3 schema may differ):
```sql
-- DF range totals (inclusive, per name)
SELECT COALESCE(n.text, s.value) AS name, COUNT(*) n, SUM(n.end-n.start)/1e6 ms, MAX(n.end-n.start)/1e6 max_ms
FROM NVTX_EVENTS n LEFT JOIN StringIds s ON s.id=n.textId
WHERE COALESCE(n.text, s.value) LIKE 'dynfilter::%' OR COALESCE(n.text, s.value) LIKE 'sirius_physical_partition::%'
GROUP BY 1 ORDER BY ms DESC;
-- kernels launched inside DF ranges (same thread, runtime call inside range -> correlationId -> kernel)
SELECT COALESCE(n.text,s.value) rng, k.shortName, COUNT(*) n, SUM(k.end-k.start)/1e6 gpu_ms
FROM NVTX_EVENTS n LEFT JOIN StringIds s ON s.id=n.textId
JOIN CUPTI_ACTIVITY_KIND_RUNTIME r ON r.globalTid=n.globalTid AND r.start>=n.start AND r.end<=n.end
JOIN CUPTI_ACTIVITY_KIND_KERNEL k ON k.correlationId=r.correlationId
WHERE COALESCE(n.text,s.value) LIKE 'dynfilter::%' GROUP BY 1,2 ORDER BY gpu_ms DESC;
```
  (`k.shortName` is a StringIds id in most schema versions -> join StringIds.) Same join against `CUPTI_ACTIVITY_KIND_MEMCPY`
  for H2D/D2H inside DF ranges (HOST bounce: report but ignore per user), and against runtime names
  `cudaStreamSynchronize`/`cudaMemcpy*`/`cudaMalloc` for sync/alloc cost.
  `~/.claude/skills/operator-perf-hunt/scripts/attribute.py <nsys.sqlite> [api,...]` does same-thread CUDA-API -> innermost NVTX attribution.
  Durations are inclusive and overlap across threads: leads, not savings.
- Iteration alignment inside one capture: no per-iteration NVTX label; split by `timings.csv` cumulative times or by gaps
  between top-level operator ranges; or capture 1 iteration per run after a warm run is impossible in-process -> use iterations=3
  and take the last window.

## 5. (d) Debug logs showing DF key outcomes

- Levels: `SIRIUS_LOG_LEVEL=debug` env (src/sirius_context.cpp:2415-2427) or `SET sirius_log_level='debug'`; dir `SIRIUS_LOG_DIR` /
  `SET sirius_log_dir`. With performance_test.py: `SIRIUS_LOG_LEVEL=debug pixi run python ... --iterations 2 --name tip_debug`
  (or `SIRIUS_PRE_SQL`). Debug/trace runs are diagnostic only (not timings). Trace adds per-operator task lines
  (`operator ... produced ... execution time: X ms`, src/pipeline/gpu_pipeline_task.cpp) and is what `log-analyzer`
  (`python3 tools/log_analyzer/parse_logs.py <log>`) requires.
- Lines to grep (per-query logs `<bench>/sirius/q<N>/sirius.log`):
  | grep | level | source | content |
  |---|---|---|---|
  | `\[dynamic_filter_key\]` | debug | src/op/dynamic_filter/dynamic_filter_key_outcome.cpp via post_unlock_actions.cpp:52 | Key Outcome: `v sig occ join condition build_ordinal rows valid kind(none/bloom/small_in_list/hash_in_list) size zone_map pushed reason` |
  | `construction started:` | info | dynamic_filter_publisher.cpp:1054 | `N key(s), M batch(es) on G GPU(s)` (M > 1 / G > 1 = multi-batch / multi-GPU build) |
  | `constructed filters published` | info | dynamic_filter_publisher.cpp:716 | filters pushed, targets, **ms after final contribution**, `(Bloom through HOST staging)` |
  | `construction declined` / `constructs no dynamic filter` | info | dynamic_filter_publisher.cpp:832, build_input_certifier.cpp:54 | declined builds + reason |
  | `construction skipped: an accumulator lease was refused` | debug | dynamic_filter_publisher.cpp:1020 | |
  | `accumulation settled: outcome=` | debug | dynamic_filter_publisher.cpp:585 | |
  | `accumulated_bloom_builder::finish` | debug | sirius_dynamic_bloom_filter.cu:1345 | root GPU, keys, bytes, peer copies vs HOST staging, sources, targets |
  | `queued .*-byte replica GPU` / `replicated .* zone` | debug | sirius_dynamic_bloom_filter.cu:629, sirius_dynamic_filter.cpp:427 | replica copies |
  | `Bloom filters refused: per-GPU cap` | info | construction_plan.cpp:231 | cap refusal |
  | `partition strategy:` | debug | src/op/sirius_physical_hash_join.cpp:851 | `N partitions (G GPUs), build side bytes/rows, Join Type, Join Mode, [broadcast]` -> single vs multi-partition |
  | `sirius_physical_partition id .* size estimate` | info | sirius_physical_partition.cpp:479-492 | partitions, sizing error |
  (Line numbers are pr2570; the S5B kit's `lib/formats.py` on branch `tmp/s5b-mgpu-experiments` has regexes for an older tip:
  `git -C /home/ubuntu/sirius show tmp/s5b-mgpu-experiments:experiments/s5b-mgpu/lib/formats.py`; its `publication:` string and some
  line numbers are stale. Kit tool `tools/dynamic_filter_key_parity.py` diffs Key Outcomes between two builds.)
- `dynamic_filter_stats` counters (e.g. `accumulation_publication_latency_ns`, `filters_skipped_construction_admission`) are C++-test only; no SQL surface.

### Which TPC-H queries build DFs; single vs multi-partition
- Not recorded locally: S5B results live only on the fork (commit fbc40f6f not in the local branch, NVMe copy wiped).
  Known from the handoff/docs: q3 join 15 builds on `orders` (147M rows, 294 MB Bloom) and filters `lineitem` (6B rows; 7.5B in -> 182M out);
  q20 Bloom -> hash IN-list at SF1; q21 does two successive reductions (docs/super-sirius/dynamic-filters.md:385); q2/q15/q21 were
  reported separately in P1; at SF10 multi-partition builds occur.
- Derive it first thing once the build exists (one debug pass, ~few min):
```bash
SIRIUS_LOG_LEVEL=debug pixi run python test/tpch_performance/performance_test.py --input $DATA --scale-factor 1000 --engine gpu \
  --config $SIRIUS_CONFIG_FILE --iterations 2 --pin host --pin-compression \
  --compression-plan-dir /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 --output $R/runs --name tip_debug
B=$(ls -dt $R/runs/tpch_*tip_debug | head -1)
for q in $(seq 1 22); do f=$B/sirius/q$q/sirius.log; echo "q$q keys=$(grep -c '\[dynamic_filter_key\]' $f) started=$(grep -c 'construction started' $f) \
 published=$(grep -c 'constructed filters published' $f) multi_part=$(grep 'partition strategy:' $f | grep -vc ': 1 partitions')"; done
grep -h '\[dynamic_filter_key\]\|construction started\|constructed filters published\|partition strategy:' $B/sirius/q3/sirius.log
```
  Expectation with `hash_partition_bytes 1GB`, 4 GPUs: builds > ~1 GB become multi-partition (hash-partitioned across GPUs), builds
  < 3 GB with a large probe may broadcast (1 partition replicated). Single-partition DF path = small dimension builds (nation/region/supplier/part filters).

## 6. (e) DF microbenchmark binary

- No google-benchmark / nvbench in the build (cuCascade benchmarks OFF; cmake/sirius-dependencies.cmake:113). Two precedents:
  1. **Catch2 `[!benchmark]` test cases inside `sirius_unittest`** (hidden by default; `test/cpp/scan/bench_decode_codecs.cpp`,
     registered at cmake/sirius-test-sources.cmake:368; `catch.hpp` = `catch2/catch_all.hpp` so `BENCHMARK` macros work; that file
     hand-rolls `bench_seconds` with `stream.sync()` timing).
  2. Standalone executables in cmake/sirius-tests.cmake (e.g. `retirer_benchmark`, lines 416-456: `add_executable` + link
     `sirius_core duckdb_static cudf::cudf rmm::rmm spdlog::spdlog cuCascade::cucascade cuCascade::cucascade_cudf
     cuCascade::cucascade_io PkgConfig::LIBURING`, `sirius_link_duckdb_extensions`, `--allow-multiple-definition`, C++20).
- **Recommended: option 1.** Add `test/cpp/operator/bench_dynamic_filter_construction.cpp` tagged `[!benchmark][dynamic_filter]`,
  add it to `cmake/sirius-test-sources.cmake` next to line 304 (unregistered test files trip `test/cpp/orphan_test_allowlist.txt`).
  It reuses the existing fixtures with zero new link work:
  - `test/cpp/operator/dynamic_filter_accumulation_test_utils.hpp`: `acc::fixture{fixture_options{.devices=N, .gpu_bytes=...}}`
    (default gpu_bytes is only 512 MiB -> raise for realistic sizes; multi-GPU via `.devices`), `fixture::certify(batches)`,
    `task_stream(i)`, `publication_stream(i)`, `gpu(i)`, session `begin(batches, cap)`.
  - `test/cpp/operator/construction_parity_utils.hpp` (`cons::join`, `cons::constructed`, `key_of`), `dynamic_filter_column_test_utils.hpp` (`make_column`, `families`).
  - Entry points (src/op/dynamic_filter/build_filter_construction.hpp:83-230): `plan_construction(inventory.facts(), admitted_keys, binding_facts, limits)`
    -> `build_filter_construction::try_create(plan, inventory, spaces)` -> per batch `reserve(batch_index, device, batch_key_facts::of(view))`
    + `contribute(ticket, view, stream, scratch)` -> `finish({.space,.stream,.scratch})`. The helpers `create/contribute_all/finish`
    in test/cpp/operator/test_build_filter_construction.cpp:~60-130 are the template (single GPU); multi-GPU / publication path templates:
    test_dynamic_filter_accumulation.cpp (`[multi_gpu]`), test_physical_partition.cpp (operator level), test/cpp/pipeline/test_in_task_publication.cpp.
  - Lower-level kernels: `detail/accumulated_bloom_builder.hpp`, `detail/bounds_accumulator.hpp`, `detail/exact_key_stager.hpp`
    (tests: test_exact_key_and_bounds_accumulation.cpp, test_accumulated_bloom_key_types.cpp).
  - Parameterize sizes via env (precedent `SIRIUS_BENCH_*_ROWS`) to hit observed shapes (e.g. q3: 147M INT64 keys, ~1 GB batches, 1-4 GPUs).
  - A microbench file is prototype tooling; if upstreamed it belongs with PR1 (#2569) for construction kernels, PR2 (#2570) for PARTITION-level placement.
- Build + run (after the background build is done; separate worktree for candidates):
```bash
pixi run cmake --build --preset release --target sirius_unittest          # binary: build/release/test/cpp/sirius_unittest
CUDA_VISIBLE_DEVICES=0 pixi run build/release/test/cpp/sirius_unittest "[!benchmark][dynamic_filter]" --benchmark-samples 20
# multi-GPU cases: leave all 4 GPUs visible
```
- nsys on the microbench (full process is fine; small):
```bash
CUDA_VISIBLE_DEVICES=0 pixi run nsys profile --trace=cuda,nvtx --sample=none --cudabacktrace=none --force-overwrite=true \
  --export=sqlite -o $R/runs/mb/df_construct build/release/test/cpp/sirius_unittest "<test name>"
bash test/tpch_performance/nsys_analyze.sh $R/runs/mb/df_construct.sqlite   # or the SQL in section 4
```
  (`--sample=cpu --cudabacktrace=all` when hunting host-side cost / who calls sync.)
- ncu on the microbench (kernel-level: occupancy, memory throughput, branch/stall; run ONE small case, ncu replays kernels and is very slow):
```bash
CUDA_VISIBLE_DEVICES=0 pixi run ncu --target-processes all --set full \
  --kernel-name regex:'bloom|insert|contains|minmax|reduce|gather|scatter|for_each|transform' --launch-count 20 \
  -o $R/runs/mb/df_construct_ncu build/release/test/cpp/sirius_unittest "<test name>"
ncu --import $R/runs/mb/df_construct_ncu.ncu-rep --page details --csv > $R/runs/mb/df_construct_ncu.csv
```
  Find exact kernel names first from the nsys sqlite (`CUPTI_ACTIVITY_KIND_KERNEL` joined to DF ranges), then tighten the regex;
  use `--nvtx --nvtx-include "dynfilter::accum::contribute/"` to restrict to a DF range (domain-qualified form may be needed:
  `<domain>@<range>/`; check `ncu --help`). ncu needs a single GPU visible and no other GPU work.
- Release build has no debug symbols/line info for ncu source view; `pixi run make relwithdebinfo` builds `build/relwithdebinfo` if needed.

## 7. (f) DF unit tests

Binary `build/release/test/cpp/sirius_unittest` (Catch2 v3 tag filter). Tests set their own memory fixtures; integration tests use
`test/cpp/integration/integration-shard.yaml` via the harness. Files and tags:

| File (test/cpp/...) | cases | tags |
|---|---|---|
| operator/test_build_filter_construction.cpp | 18 | `[dynamic_filter][construction]` (+`[tracked]`,`[in_list_bound]`) |
| operator/test_dynamic_filter_accumulation.cpp | 52 | `[dynamic_filter][construction]`, 14 `[multi_gpu]` |
| operator/test_dynamic_filter_accumulation_fault.cpp | 5 | `[cuda_status_fault][multi_gpu]` |
| operator/test_exact_key_and_bounds_accumulation.cpp | 9 | `[construction][bounds]/[stager]` |
| operator/test_accumulated_bloom_key_types.cpp | 5 | `[construction][bloom]` |
| operator/test_dynamic_filter_construction_plan.cpp | 16 | `[construction_plan]` |
| operator/test_complete_build_inventory.cpp / test_build_input_certification.cpp | 15 / 7 | `[inventory]` / `[certification]` |
| operator/test_dynamic_filter_key_outcome.cpp / _prepared_keys.cpp | 12 / 6 | `[key_outcome]` / `[prepared_keys]` |
| operator/test_dynamic_filter_publisher.cpp / _publication_claim.cpp | 44 / 11 | `[publisher]` / `[publication_claim]` |
| operator/test_physical_partition.cpp | 35 (16 DF) | `[physical_partition]` |
| operator/test_physical_build_filter.cpp | 10 | `[build_filter]` |
| pipeline/test_in_task_publication.cpp | 10 | `[in_task_publication]`, 4 `[multi_gpu]` |
| integration/test_gpu_execution_dynamic_filter_{sip,multi_partition,native,domain_evidence}.cpp | 18/3/.. | `[integration][gpu_execution][dynamic_filter]` |
| operator/test_sirius_dynamic_filter_mgpu.cpp, test_physical_hash_join_mgpu.cpp | 10 / 18 | `[multi_gpu]` |

```bash
# fast single-GPU DF loop
CUDA_VISIBLE_DEVICES=0 pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]"
pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter][construction]"          # construction only
pixi run build/release/test/cpp/sirius_unittest "[physical_partition]"
# multi-GPU DF tests (all 4 GPUs visible)
pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter][multi_gpu]"
# full harness (shards 2/GPU, then context, cuda_fault, multi_gpu, late_mat); logs build/release/test/cpp/log/<process>/
pixi run make test                      # or: pixi run python scripts/run_unit_tests.py --build-dir build/release --steps multi_gpu -- "[dynamic_filter]"
```
Caveats: `make test` rebuilds first; S5B C1 found 2 stale `[multi_gpu]` tests on an older tip (log string `dynamic-filter publication:`,
16 MiB scratch bound) - recheck on pr2570 before treating failures as regressions; `SIRIUS_TEST_REQUIRE_POOL_PEER_ACCESS=1`
makes peer-copy tests fail instead of skip (this box lacks P2P across GPU pairs -> keep unset).

## 8. Reusable knowledge from the S5B kit (branch `tmp/s5b-mgpu-experiments`, local tip 28d82c42; results commit fbc40f6f not local)

- Session recipe (`lib/session.py`): one DuckDB CLI process; LOAD; views; `CALL pin_table('<glob>', tier => 'host', name => '<table>')`
  for every table; pass 0 warm at debug/trace with result check; 5 hot passes at info with `.mode trash`, `.timer on`; per-run
  `SET sirius_log_dir` + `SET sirius_log_level`; query order rotated between passes; latency = median of 5 hot.
- A run counts only if its window ends `[window] end ... outcome=ok`, no fallback, >= 1 `assigned pinned entry`.
- Noise handling: base_a / tip / base_b sessions (A/A pair), confirm any query > 1.10x in fresh processes. P1 result on old tip:
  4-GPU geomean 1.0096, 1-GPU 1.0086 with q3 1.14x (cap artifact, now moot per section 1).
- P4 metric worth reusing: publication latency after the final Contribution (p50/p95/max) from the info `published` line, warm vs hot.
- Metrics unavailable from Sirius: DF counters via SQL; IN-list / zone-map replica copy path (only Bloom logs transport).
- Skills: `operator-perf-hunt` (`~/.claude/skills/operator-perf-hunt/references/measurement-recipes.md`: identity/inventory, B,C,C,B
  blocks, GPU lease, microbench pricing table), `optimize-blind` (campaign layout 00-baseline/10-analysis, pathology checklist),
  repo skills `profile-analyzer`, `optimization-advisor`, `log-analyzer`, `benchmark`.

## 9. Open questions
1. Regime the user validated (host-pinned compressed vs disk parquet)? Section 2 assumes `--pin host --pin-compression`.
2. Quent output lands on the root disk (11 GB free) and cannot be redirected without a config copy - OK to prune old sessions or use a
   copy that differs only in `telemetry.output_directory`?
3. Data layout under `$DATA` (single file vs per-table dir) is unknown until datagen finishes; view/pin globs must match it.
