# Deferred read ordering: architecture and measurement map

Read-only investigation, 2026-10-15. Production files were not edited and no GPU work was run.

## Most important result

The supplied primary configuration reaches deferred vectored reads, but **does not enable local readahead**. `src/io/uring/config.hpp:34` defaults `n_max_concurrent_scans` to zero. `templated_ioctx.hpp:217` returns that value directly. `scan_manager_config::resolve_readahead` (`src/scan_manager/config.hpp:281`) honors a zero backend budget before substituting pipeline width. With no explicit `max_readahead_scans` in `/tmp/tpch-deferred-order-20261015/config.yaml`, no readahead manager is created (`sirius_scan_manager.cpp:1545`). Thus normal query-created active-prefetch waits should be zero by construction in this primary experiment. This is useful production-default evidence; it is not evidence that prefetch can never block with other configurations.

The local backend's source comment attributes its default-off choice to earlier SF1000 measurements; that is historical rationale, not a measurement performed in this investigation.

## Exactly what makes A block

`deferred_device_batch::await` drains the cache registry for the explicit CUDA stream, issues other live batches in registration order, then issues its own batch and waits on its shared completion (`src/io/deferred_device_batch.cpp:63`). A batch contains one datasource's pending slices for one stream and submitting device. A datasource generally represents one file slice in the coalesced scan; each split gets a duplicated datasource and independent prefetch handle (`parquet_gpu_ingestible.cpp:414`). Other futures in the same batch do not constitute sibling batches.

`issue()` synchronously enters `sirius_datasource::device_read_ranges_async`, which synchronously executes cache preparation before returning its future. `prefetching_cache::await_inflight_prefetch` (`src/io/cache/prefetching_cache.cpp:785`) waits only when all of these hold:

1. The datasource has a valid prefetch handle.
2. At least one chunk in that handle overlaps a nonempty valid requested byte range and is currently in the `loading` chunk state.
3. The handle's own producer state is `loading` (`cache_handle::is_prefetch_in_flight`, line 218).

It then waits for the **whole handle's producer** to leave loading (`wait_until_ready`, line 228; `producer_stage::wait_till_not_loading`, `types.hpp:761`), not just the intersecting chunk. A successful completion, failed load reverting to prepared, or abandonment wakes the waiter. A producer that completes between the observations and the wait can make this call return immediately. Therefore count the predicate separately from materially positive elapsed wait time.

A cache miss alone does not cause this wait. A pending allocation, queued prefetch that has not started, unrelated loading ranges, and demand-owned loading chunks without an active handle producer do not meet it. Those go through cache/demand-read arbitration and can issue bounce-staged reads instead. Registry/batch mutex contention and backend submission overhead can also lengthen issuance; do not label all issue time as prefetch blocking.

Real reasons for meeting the predicate: consumer catches an already-issued prefetch before it completes, e.g. late readahead start, short lead time, a large selected range set, storage/queue contention, or an enabled prefetch consuming shared backend capacity. These are causal possibilities; their frequency must come from direct diagnostics.

## Scheduler triggers: source is more complete than docs

The current source is more precise than the configuration documentation's “one prefetch per non-scan task” description. With a positive budget:

- Eager strategy calls `arm_prefetching()` immediately during `start()`.
- Opportunistic strategy first arms on a non-`GPU_SCAN` task deployment, an empty scheduler task queue, a memory-downgrade event, or a wait-for-memory event (`readahead_scan_manager.cpp:58–118`).
- `arm_prefetching()` is **one-time**: `_prefetching_started.exchange(true)` then `_gatekeeper.reload()` (`:467`). Subsequent trigger calls do not grant more credit. The continuous worker issues subject to the gate budget and recycles tickets thereafter.
- Pure scan queries can still trigger via scheduler-empty or memory events. It would be incorrect to claim they cannot prefetch because they lack a non-scan stage.

A useful explicitly labeled sensitivity is `max_readahead_scans: 8` with `readahead_strategy: eager`, followed by the same workload with `max_readahead_scans: 0` as causal control. An opportunistic positive-budget diagnostic can additionally distinguish startup strategy if needed; do not enlarge timing trials based on favorable significance.

## Reachability and useful diagnostic counters

The primary config sets local Sirius/uring, `cache.mode: sirius`, deferred reads true, and unpinned scans. This reaches cuDF's general reader: uring advertises `prefers_bulk_io=false`; REST advertises true and the usual REST bulk materializer explicitly calls vectored reads and bypasses this batching policy (`parquet_materialize.cpp:74,158`; `parquet_gpu_ingestible.cpp:1723`). KvikIO has no armed prefetching cache and bypasses deferred batching. Null/legacy/per-thread-default streams are eager; explicit streams are required.

Record by query/cache regime, and if possible group id, datasource id/path, stream and device:

- first await on an open batch versus member awaits on already-issued batches;
- live *open* siblings actually transitioned to issued, separately from expired/already-issued registry entries and self;
- singleton versus multi-source drain groups, maximum and distribution of open group size;
- A's overlapping-loading-plus-producer-loading predicate **at first-await entry before any sibling issuance**;
- the actual same predicate inside each issue, separately own versus sibling;
- actual prefetch-wait count, total/max/histogram duration, and durations over an explicit threshold such as 10 microseconds;
- issue duration excluding/including prefetch wait, and first-await wall duration;
- optional selected bytes/ranges per batch, cache hits/misses/loads, deferred flushes, and source count per cuDF materialization.

The entry snapshot matters: under siblings-first A's prefetch can complete while another sibling issue blocks. Measuring only the later own issue would undercount A's initial blocking opportunity. Count open groups, not every member future, in the denominator. Cache-wide `wait_for_prefetch` telemetry only observes `scan_stage::reading` at prefetch completion; it does not prove that A actually entered this wait.

The installed cuDF headers state that consuming the returned device-read future synchronizes the read, but they do not establish which file's future the reader first waits on. Searches found no local cuDF implementation sources. Prior disassembly `/tmp/cudf-fetch-disassembly.txt` contains the expected device-future vector path but is insufficient here to assert exact current file wait ordering. Upstream main reader sources were consulted, but they are not pinned to the installed `libcudf 26.08.01`. Treat first-wait source ordering as an empirical diagnostic; do not claim A is predictably the slowest file or attribute the policy to a cuDF global mutex.

## Data and query envelope

- Existing `/data/tpch_parquet_sf10`: one file per table; useful small sanity test but generally no cross-file siblings in a single-table reader call.
- Existing `/data/tpch_parquet_sf100`: six lineitem files, two orders files, one part file. Best tractable original-layout experiment for the requested subset.
- Existing `/data/tpch_parquet_sf1000`: about 60 lineitem files, 15 orders files, two part files. Larger follow-up if justified; not needed to manufacture an effect.
- Existing harness `test/tpch_performance/run_tpch_parquet.sh`, SQL `test/tpch_performance/tpch_queries/orig/q{1,6,14,19}.sql`.
- Q1 is a wider lineitem scan with grouped aggregation; Q6 is a narrower filtered lineitem aggregate; Q14 adds a part join and date restriction; Q19 adds part join with disjunctive/string filtering. Use repository SQL constants unchanged.
- Several files in a table do not guarantee several files in one deferred drain: `scan_task_batch_size`, selected columns, file boundaries, row-group pruning, and cuDF passes determine coalescing. Joins alone do not create siblings; their tasks normally use different explicit streams. Report zero-sibling observations honestly.
- The supplied 1536Mi scan target and 64Gi host capacity are explicit campaign settings, not defaults. They are structurally valid; runtime memory/reservation and pinned-pool footprint must still be checked by the runner. Do not reuse GB300-specific `bench/sf1000-repro` YAML for this L40S.

## Cache resets

`CALL reset_sirius_cache()` requires initialized healthy Sirius context and acquires its execution `SlotGuard` (`sirius_extension.cpp:1497–1520`). `sirius_scan_manager::reset_caches` (`:2271`) drops parquet-tier pins, drains and destroys each ioctx's cache, and recreates eligible caches. This also resets cache counters and deferred registry. It keeps the ioctx itself, its separately owned parsed `metadata_store`, CUDA context, allocators, and process/runtime warm state. It does not drop OS page cache. With the current cache mode, data reads use O_DIRECT where supported; “fresh Sirius cache” and “retained Sirius cache” are the right labels, not cold/hot disk.

## Recommendation before query measurements

Do not change global ordering from the synthetic matrix alone. Its useful result is conditional: sibling-first exposes work while own issue is blocked; own-first can reduce requested-source latency; total reader completion often needs all files. The production-default local path currently cannot realize the advertised prefetch-overlap benefit because readahead is off. First establish open-sibling frequency and fixed-sample query latency. If primary timings are indistinguishable, avoid a new runtime tuning knob or adaptive predictor. If a policy change is proposed later, preserve the distinction between making *all* independent batches issuable at first wait (mechanism) and which one is synchronously submitted first (policy).

## Assigned reading receipt

Actually read for this investigation, not claimed as entire books:

- Repository `AGENTS.md`; `/home/ubuntu/AgentDocs/codex-agents/architect.toml`.
- `A Philosophy of Software Design.md`: 4.1–4.4 (lines 1058–1240) and 5.1–5.2 (1432–1520), module interfaces, deep modules, information hiding/leakage.
- `C++ Software Design.md`: G4 opening/problem and concluding separation-of-concerns discussion (1888–1950,2160–2245); G6 abstraction expectations introduction and conclusions (2641–2720,3020–3070).
- `C++20 -- The Complete Guide.md`: ranges/view introduction and constrained algorithm example (6500–6585); span fixed/dynamic extent and lifetime example (11520–11640). No new production interface was designed.
- Super Sirius README reading order; architecture overview, execution-flow opening; scan operator/coalescer/read-path/cache sections; relevant memory-management/reservation sections and configuration cache/readahead sections. Initial combined output was truncated; key relevant sections were reread in focused excerpts and source inspected directly.
- Production datasource/batch registry, cache handle/wait/arbitration, producer-stage atomics, scan coalescer/materializer, readahead manager/config, reset path, installed cuDF datasource contract, benchmark methodology/results, and current experiment contract/config.
- Reference implementations: local DuckDB parquet reader prefetch selection and per-column/whole-group scheduling (`duckdb/extension/parquet/parquet_reader.cpp:48,1430–1500`); [Velox BufferedInput](https://raw.githubusercontent.com/facebookincubator/velox/main/velox/dwio/common/BufferedInput.cpp) enqueue/load/read latency accounting (84–158); [DataFusion stream wrappers](https://raw.githubusercontent.com/apache/datafusion/main/datafusion/physical-plan/src/stream.rs) bounded producer channel and task lifecycle (41–157). These support keeping submission/batching and completion/lifetime distinct; they do not dictate a Sirius ordering policy.
- No `/module-context` command/skill found by the earlier parent search or this scoped filesystem search. No production implementation performed; installed headers and exact local source were used for contracts. Upstream cuDF main sources are background only, not installed-version proof.

## Interpretation after the SF100 diagnostics

Independent inspection of `/tmp/tpch-deferred-order-sf100-20261015/diagnostic_summary.csv` confirms the supplied sibling-first, fresh-cache totals with explicit prefetch budget eight: Q1 A pending 48/54 first-await groups (88.9%), Q6 18/33 (54.5%), Q14 20/33 (60.6%), Q19 65/66 (98.5%). These are conditional frequencies for this enabled-prefetch configuration and instrumented workload. Default local readahead remains off, with zero pending-prefetch waits observed; retained-cache runs also have zero in both configurations. They must not be presented as the ordinary default probability of blocking.

Across the 60 multi-source first-await groups in those sibling-first fresh-cache runs, 44 have both A and a sibling pending, zero have only A pending, one has only a sibling pending, and 15 have neither. Each measured multi-source group has one sibling: 60 actual sibling issues for 60 multi-source groups. Consequently **the clean synthetic scenario—A's prefetch blocks while B/C are independent demand misses ready to launch—was not observed at first-await entry**. This weakens the synthetic rationale's applicability to this particular real workload; it does not invalidate the mechanism demonstrated by the microbenchmark.

The relevant production stages are:

1. The readahead thread enters `prefetching_cache::prefetch`, marks the producer loading, claims cache chunks, builds host-only prepared slices, and dispatches them independently through `host_device_readv_async_io` (`prefetching_cache.cpp:1307–1394`). The batch await is not what launches this prefetch.
2. Deferred batch issue first calls `await_inflight_prefetch` (`:1028`) and can block before preparing any of its own cache copies or backend requests.
3. When the prefetch finishes, loaded chunks are published cached and the producer becomes ready (`:1366–1383`). The waiting issue then pins covering cached chunks and collects H2D copies (`:1067–1075`). Only gaps/unavailable coverage become fresh backend requests (`:1078–1093`), dispatched if `has_backend` is true (`:1139`). Cached copies are enqueued separately (`:1149`, `:1157–1213`).

Therefore, when both A and B have pending prefetch, calling B's issue first often does **not** start another storage read; it waits for an already-initiated producer and then starts B's H2D stage. A's prefetch continues independently while that wait happens. The counters prove that the producer has entered loading and an overlapping chunk is loading, not that a physical disk request is already executing: producer/chunk loading transitions occur just before backend dispatch. Precise request-queue ordering would require backend trace events.

Several meaningful differences remain possible:

- **Earlier H2D for the first-ready producer.** If B's prefetch finishes before A's, sibling-first can copy B while A is still loading. Own-first can withhold that copy while waiting on A. If A finishes first, the reverse order exposes the equivalent opportunity. Neither fixed source order knows which producer will finish first.
- **Demand misses or partial coverage.** Pending overlap says that at least one requested chunk is loading, not that every requested byte will be cached after the prefetch. Once a batch passes its wait, it may still dispatch new reads for gaps. Their launch order and the shared reactor queue can matter. The current counters do not establish how many such bytes existed in these groups.
- **No-prefetch or fully retained data.** With no blocked producer, policy mainly reorders demand submissions or cached H2D copies. The same stream serializes these cached copies, and the Parquet reader ultimately needs all required sources; advancing A's individual future does not necessarily advance query completion.
- **Interaction with the rest of the query.** Concurrent scan tasks use other streams and compete for backend/PCIe capacity. Earlier source completion can alter task release, cache residency, and downstream scheduling. These can change end-to-end time without proving the synthetic blocked-A explanation.

The own-versus-sibling wait totals also redistribute under a policy change. A lower sum attributed to `own` in sibling-first is not automatically saved time: some waiting moved to the sibling, and waits across concurrent scan tasks overlap in wall time. Use completed uninstrumented end-to-end timings for the performance decision.

### Provisional decision options

- Keep deferred grouping and first-wait issuance of all relevant source batches: these are distinct from the choice of which batch goes first, and this investigation is not an ablation of batching itself.
- Do not use high A-pending frequency alone to justify sibling-first. The joint state is the relevant observation, and here it overwhelmingly says both sources are already being prefetched.
- Do not switch order merely because own-first is more intuitive or because it improves isolated A-ready latency. Wait for the prespecified query results. If one order shows a consistent worthwhile improvement without material regressions in the intended configuration, a small fixed-order change is preferable to a new runtime tuning knob.
- If timings are within noise or have mixed tradeoffs, retain the current order and explain its limited evidence. The strongest immediate recommendation is to keep default local prefetch disabled and avoid enabling it solely to justify this ordering heuristic. Whether enabling prefetch improves a different storage/workload envelope is a separate question.
- A future mechanism that can arrange all independent submissions without synchronously waiting on one source could remove this choice, but it adds completion/lifetime/concurrency work. The present observations do not by themselves justify that implementation. No adaptive policy, new queue, or production edit is proposed here.

Additional reading for this interpretation: exact `prefetch()` dispatch/completion path, device-read cache hit/miss preparation, cached-copy retirement submission, and the completed diagnostic CSV. No GPU execution or production edits were performed.
