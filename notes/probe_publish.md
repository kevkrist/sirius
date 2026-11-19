# DF publication + probe (consumer) side at stack tip (pr2570 @ ae939824)

Read-only code/doc survey. Base = c4fa3638 (merge-base origin/main). Paths relative to repo
`/opt/dlami/nvme/dfperf/repo`. Nothing here is measured; hypotheses are marked H#.

## 0. Corrections to the task premise (important)

- **The "256 MiB default cap" no longer exists at tip.** `max_dynamic_filter_bloom_bytes_per_gpu`
  is not a config key any more (YAML use throws: `src/sirius_config.cpp:680-684`). It is derived:
  `max(4 * scan_task_batch_size, max_build_hash_table_bytes)` (`src/sirius_config.hpp:272-275`).
  - base and pr2569: `4 * scan_task_batch_size` only; the `max(...)` is added by 2570 commit 9a5069bf.
  - Under `specific_config.yaml` (scan_task_batch_size 4GB, max_build_hash_table_bytes 8GB) the cap
    is **16 GiB at base, 2569 and tip**. A 294 MB q3 Bloom is far below it and should NOT be refused.
  - The earlier "q3 refused, 1.14x on 1 GPU" finding (memory note s5b-mgpu, 2026-11-14) was made with
    an explicit `S5B_MAX_BLOOM_BYTES_PER_GPU` knob on an older stack revision. Treat it as stale;
    re-verify with `[plan_construction] Bloom filters refused` INFO log line
    (`src/op/dynamic_filter/construction_plan.cpp:233-238`) and counter `keys_skipped_bloom_size_gate`.
  - Standalone fallback constant is 3.2 GiB (`src/op/dynamic_filter/config.hpp:35`), not used by planned queries
    (planner passes the derived value: `src/planner/sirius_plan_comparison_join.cpp:852`).
- Cap semantic that DID change: base applied the cap only to multi-partition accumulation; the
  whole-build (single-partition / broadcast) Bloom built at CONCAT was uncapped. Tip caps every build
  (doc `docs/super-sirius/dynamic-filters.md:352`). Cap refusal drops ALL Bloom keys of the join, not just the
  largest (`construction_plan.cpp:219-242`). Irrelevant at 16 GiB unless a build has > ~8e9 key-rows.

## 1. What the stack changed on this side (by PR)

Consumer/probe side is **untouched**: `git diff base..pr2570 -- src/op/scan` is empty; probe kernels
(`membership_probe_functor`, fused kernel, bloom/set/needle lookups) unchanged. Only additions in
`src/cuda/dynamic_filter_probe.cuh` are build helpers (`require_prepared`, `current_device`,
`with_string_fingerprint_iterator`), all 2569.

| Area | base | 2569 (S5B1) | 2570 (S5B2) |
|---|---|---|---|
| Single-partition / broadcast build | whole-build one-shot at CONCAT->join "build" deposit (`sirius_physical_hash_join::push_data_batch_partitioned` -> `observe_whole_build`), all filter kinds, from the one concatenated batch, after deposit | unchanged (one-shot still there) | **moved to build PARTITION** forward path (`src/op/sirius_physical_partition.cpp:158-197`), per-batch contribute + publish-before-deposit; one-shot deleted (5ccdcfcd) |
| Multi-partition hash build | Bloom-only accumulation at PARTITION, INT32/INT64 keys only | accumulation generalized: construction plan per key -> small IN-list / hash IN-list / Bloom / zone map, every key type (strings, decimals, dates), from certified inventory (`build_filter_construction`, `exact_key_stager`, `bounds_accumulator`) | same path now used for every layout |
| RIGHT_SEMI/ANTI BUILD_FILTER stage | Bloom INT32/64 only | - | every key type + kind (25a98381) |
| Bloom cap | 4x scan batch, multi-partition only | same | max(4x scan batch, max_build_hash_table_bytes), all builds |
| Config | `enable_dynamic_filter_multi_partition` | same | removed (throws if present) |
| Session API | observe_whole_build + try_begin_accumulation | both | `begin/decline/contribute/publish_if_final/finalize_input` only (`dynamic_filter_publisher.hpp:63-214`) |
| Observability | - | `[dynamic_filter_key]` DEBUG line per key at terminal (`dynamic_filter_key_outcome.hpp`), `post_unlock_actions` (logs after mutex release) | per-reason decline counters (`dynamic_filter_stats.hpp`) |
| PARTITION memory estimate | forward layout = 0 | same | forward layout = 2x input + `task_scratch_bound()` while constructing (`sirius_physical_partition.cpp:506-518`) |

Ownership of optimizations: anything in the accumulate/merge/replicate/finish machinery, key planning,
exact staging, bounds = **2569**. Anything about construction at single-partition/broadcast PARTITION,
forward-path contribution, PARTITION task estimate, cap formula, BUILD_FILTER all-kinds = **2570**.
Probe kernels / consumer gate / bloom geometry = pre-existing (neither PR; note separately).

## 2. Publication pipeline at tip

Session = `dynamic_filter_publication_session` (`src/op/dynamic_filter/dynamic_filter_publisher.cpp`), 1 per join.
Phases OPEN -> COLLECTING -> PUBLISHING -> TERMINAL.

1. **Certify + begin** (task-creator thread, before first PARTITION claim): `hash_join_partition_observer::before_claim`
   -> `build_input_certifier::decide` -> `session.begin(inventory)` (`publisher.cpp:943-1077`, NVTX
   `dynfilter::construction::begin`). Needs FULL barrier + finished source pipeline (build PARTITION input
   barrier is FULL: `src/op/partition/consumer_partition_policy.cpp:62`). `plan_construction` picks per key
   (`construction_plan.cpp:104-246`); `build_filter_construction::try_create` leases, **on every replica GPU**,
   zeroed Bloom arrays for every Bloom key + exact staging buffers + bound slots (non-blocking leases; refusal
   => whole attempt skipped `accumulations_skipped_admission`).
2. **Contribute** (each PARTITION task, before scatter / before forwarding): `session.contribute`
   (`publisher.cpp:1095-1170`). Under mutex: claim original id, `reserve` (host arithmetic). Outside: Bloom
   `add_async`-style insert into the partial array on the batch's GPU (`sirius_dynamic_bloom_filter.cu:1080-1121`,
   NVTX `dynfilter::accum::contribute`), exact keys staged (`dynfilter::stage::contribute`), bounds reduced
   (`dynfilter::bounds::contribute`). No host wait except STRING bounds. Requires GPU representation of the
   batch; otherwise attempt ends `skipped_inventory` (`publisher.cpp:1136-1143`). If all target channels are
   already closed the attempt ends early (`end_if_channels_closed`, `publisher.cpp:1128`) - good.
3. **Publish** (the task whose contribution completed the inventory, after its output is built, **before
   deposit**): `publish_if_final` -> `state::publish` (`publisher.cpp:627-769`):
   - `build_filter_construction::finish` (`build_filter_construction.cpp:305-400`): bounds finish (1-2 host
     syncs, `bounds_accumulator.cu:439,518`), Bloom finish (below), exact gather (sync per non-root source +1,
     `exact_key_stager.cu:279,316`), build IN-lists/zone maps on root under room check
     (`get_available_memory`, :343), final `root.stream.sync()` (:398).
   - replicate exact IN-lists + zone maps to other GPUs (`replicate_to_devices`, NVTX
     `dynfilter::replicate_devices`; one `cudaStreamSynchronize` per filter: `sirius_dynamic_in_list_filter.cu:368`,
     `sirius_dynamic_small_in_list_filter.cu:322`).
   - optional cross-rank merge (distributed only; `cross_rank_filter_merge.hpp`; not used single-process).
   - `fan_out` (`publisher.cpp:273-329`): allocation-free pushes to each accepting channel/binding. NVTX marks
     `dynfilter::accum::final_commit` and `dynfilter::accum::visible`; INFO log "constructed filters
     published ... N ms after the final contribution" (`publisher.cpp:715-723`); counter
     `accumulation_publication_latency_ns`.
4. `finalize_input` after site tasks drain; storage release separate.

### Bloom merge + multi-GPU distribution (`accumulated_bloom_builder::finish_impl`, `sirius_dynamic_bloom_filter.cu:1130-1357`)

- Every replica GPU owns a full-size array per Bloom key from `begin`. Root = publishing task's GPU.
  `sources` = non-root GPUs that contributed; `targets` = all non-root GPUs.
- Transport: PEER_COPY iff every pool pair has peer access (`find_pair_without_pool_access`, :1191), else
  HOST_STAGED (this machine; user says ignore the bounce itself).
- HOST_STAGED loop (:1285-1320) per key, per <= 8 MiB chunk, strictly serial with a host
  `cudaStreamSynchronize` after every leg: for each source {D2H+sync, H2D to root scratch+sync, OR kernel};
  then if targets {D2H root chunk+sync, H2D submitted to all targets, wait each}. So a B-byte Bloom with
  s sources, t targets moves (2s + 1 + t)*B bytes over PCIe in ceil(B/8MiB) rounds and
  ~ (2s + 1 + t) syncs per round. 4 GPUs, all contributing: 10*B bytes, 10 syncs/chunk; a 294 MB Bloom
  = 36 chunks, ~2.9 GB of traffic, ~360 host syncs, all on the critical path of the final PARTITION task.
- With 1 GPU: no sources/targets => merge is free. Single-GPU regressions cannot come from transport.
- Filters are wrapped without copying (`make_filters`, :1359-1383); consumers read the local replica only.
- Exact IN-list / zone-map replicas: built on root, copied per target via `enqueue_replica_copy`
  (`src/cuda/dynamic_filter_replica_transfer.cu:40-62`), peer or whole-transfer HOST staging.

## 3. Channel, readiness, waiting

- Channel `sirius_dynamic_filter_set` (`src/op/dynamic_filter/sirius_dynamic_filter.hpp`): append-only,
  `snapshot()` under one lock returns owning immutable entries + pending/terminal state. **Not a barrier.**
- **Consumers never wait** (doc `dynamic-filters.md:83`, `:324`). A batch processed before publication is
  never revisited; a late filter only helps later batches. No timeout exists because there is no wait.
- Why filters are usually on time anyway (demand-driven scheduling):
  - Only `scans.front()` is started at query start (`src/pipeline/task_scheduler.cpp:235`); other scans are
    activated through `get_next_task_hint` chains (`src/creator/task_creator.cpp:324-354`).
  - Hash join hint asks for the build before the probe (`sirius_physical_hash_join.cpp:968-998`, wait_for_build
    then wait_for_probe; STANDARD/MIXED: `:999-1023`), build PARTITION has a FULL barrier, and BUILD_PROBE's
    build CONCAT is `concat_all`. So the probe-side scan pipeline is first demanded after the build CONCAT
    push, which follows the elected PARTITION task's deposit, which follows publication.
  - => for immediate BUILD_PROBE probes, **publication time is pure serial latency on the critical path**
    (it delays the deposit), not lost coverage.
- Where filtering can be lost / partial (doc table `dynamic-filters.md:317-322`):
  - a target scan that happens to be `scans.front()` or was activated by an unrelated hint chain;
  - STANDARD / MIXED_JOIN joins (probe not held behind the build edge);
  - transitive targets through an intervening join;
  - cached compressed scans whose decode was prefetched/prepared before publication: decode-time probes are
    chosen at preparation (`src/op/scan/sirius_gpu_scan_operator_data.cpp:110-135`), "prefetched or reused
    converted output ... retain the existing history" (doc :177) => those chunks are filtered only at the
    endpoint (full decode + materialize first);
  - Parquet reader zone maps: "splits capture dynamic filters at emission" (`docs/super-sirius/scan.md:844`),
    snapshot at `src/op/scan/parquet_scan_core.cpp:700,2670` (zone maps are off by default anyway).
- Timing change from the stack (single-partition/broadcast builds): base published after the CONCAT output
  was deposited into the join; tip publishes in the last PARTITION task, before CONCAT runs. That is
  **earlier** in pipeline order, so coverage for racing/transitive consumers can only improve; for immediate
  probes both are before probe activation. Multi-partition timing is unchanged vs base (already at PARTITION).

## 4. Consumer / probe side (unchanged by the stack; context for profiling)

Application sites (all take one snapshot per batch):
- endpoint operator `sirius_physical_dynamic_filter::execute` (`src/op/scan/sirius_physical_dynamic_filter.cpp:82`,
  NVTX `sirius_physical_dynamic_filter::execute`, `dynfilter::apply_output`);
- resident Parquet checkpoint inside the scan (`src/op/scan/sirius_gpu_scan_operator.cpp:483`);
- cached compressed decode probes (`sirius_gpu_scan_operator_data.cpp:70-135`; relevant here:
  `enable_pin_table_compression: true`), capped by `fused_scan_max_membership_sources`;
- core logic `src/op/scan/dynamic_filter_merge.cpp` (`prepare` :1352, `select_for_decode` :1291, mask steps
  :736-1023).

Kernels:
- ordinary: `run_membership_probe` (`src/cuda/dynamic_filter_probe.cuh:328-357`) = `cub::DeviceFor::Bulk`
  over rows writing a BOOL8 (1 byte/row) mask, optional prior mask; then cudf apply_boolean_mask / gather.
  One kernel + one mask column per filter (cascade), or deferred-keys / gather-once strategies (doc :165-167).
- fused: `try_fused_membership_mask` (`src/cuda/dynamic_filter_mask_ops.cu:143-198`): up to 4 filters per
  kernel round (`fused_membership_round_capacity`, `dynamic_filter_mask_ops.hpp:33`), signed i32/i64 reps
  only; strings/unsigned/DECIMAL128 fall back to the ordinary chain.
- Bloom lookup = `cuco::bloom_filter_ref::contains` with `cuco::bloom_filter_policy<Key>`
  (`src/cuda/dynamic_filter_fused_probe.cuh:76-80,112-120`): 256-bit block (8 x u32), one 32-byte block read
  per probe, 8 pattern bits. Sizing `bloom_sizing.hpp:33-45`: 16 bits/key => 16 keys/block.
  Estimated FPR ~1e-3. A 294 MB array is far beyond L2, so each probe is one random DRAM read.
- Hash IN-list = cuco static_set, chosen only if estimated set bytes <= `dynamic_filter_inlist_max_l2_fraction`
  (0.125) of the smallest probe-GPU L2 (`dynamic_filter_source_policy.hpp:78-92`; L2 read per replica GPU at
  `publisher.cpp:212-234`). Small IN-list for 1-12 keys.
- Multi-key joins get one independent filter per key column (no composite-key filter).

Gates (`src/op/scan/dynamic_filter_gate.hpp`): per-scan gate UNKNOWN -> ACTIVE/DISABLED from one combined
sample (keep > `dynamic_filter_keep_threshold` 0.9 disables until the channel grows); per-filter skip when
its marginal keep > 0.5 (0.6 for rows >= 64 B wide). Construction never judges selectivity
(doc :357): a useless Bloom is still built, merged, replicated on 4 GPUs, then discarded by the gate after
one sampled batch. Only the domain-coverage gate (needs uniqueness proof / `unique_cols` pin declaration,
threshold 0.9) and plan-time build evidence prevent construction.

Multi-GPU on the consumer: only a replica on the batch's own device is used; a GPU without a ready replica
skips the filter; a batch held on another device passes unfiltered (`applications_skipped_other_device`).

## 5. Performance hypotheses (to test; none measured here)

Publication latency (critical-path for immediate probes; owner PR in brackets):
- H1 [2569] HOST_STAGED Bloom merge is fully serial with ~10 host syncs per 8 MiB chunk
  (`sirius_dynamic_bloom_filter.cu:1285-1320`). Independent of the bounce itself, mechanics could be improved:
  larger chunks / multi-chunk windows, overlap source D2H legs across GPUs (they are independent streams),
  pipeline D2H of chunk i+1 with OR of chunk i, drop per-leg syncs in favour of events. Measure with NVTX
  `dynfilter::accum::finish` and the "N ms after the final contribution" log line per query.
- H2 [2570] Single-partition/broadcast builds now pay the multi-GPU accumulate path: arrays zeroed on all
  4 GPUs + cross-GPU OR when the build's batches sit on several GPUs, where base did one insert pass over one
  concatenated batch + 3 replica copies. For small builds this is latency per join (many joins per query),
  dominated by fixed costs: leases, memsets, host syncs in `finish` (bounds x1-2, gather, final sync,
  per-filter replicate sync). Count syncs per publication; small-build queries (q2, q5, q8, q11 dims) are the
  ones to check.
- H3 [2569] Every Bloom key leases a full array on every GPU at `begin`, held until site finalize, even if a
  GPU never contributes or never probes. Memory = keys x bytes x 4; unspillable. Could cost reservation
  pressure / OOM-retries of scan or partition tasks on big builds (q3/q10/q18/q21 class).
- H4 [2570] Forward-layout PARTITION tasks now reserve 2x input + scratch instead of 0 while constructing
  (`sirius_physical_partition.cpp:506-518`): may throttle concurrency or trigger downgrade for
  single-partition builds. Also a forward batch not on GPU kills the whole attempt (`publisher.cpp:1136-1143`);
  check `accumulations_skipped_inventory` on TPC-H.
- H5 [2569] Contribution kernels run on the PARTITION task's stream before scatter; Bloom insert of a batch
  into a >L2 array is random-write bound. Per-batch cost adds to every PARTITION task (not only the last).
  Compare PARTITION task time base vs tip (Quent) on multi-partition builds; base did the same for
  INT32/64, so the delta should come only from new key kinds (strings hashed in-kernel, bounds, exact staging).

Filter quality / selection:
- H6 [2569] Multi-partition builds now get exact IN-lists and non-integer keys (base: Bloom INT32/64 only).
  More filters published => more probe work; net effect per query unknown. `[dynamic_filter_key]` DEBUG lines
  give kind/size/pushed per key for base-vs-tip diffing (key identity `sig/occ` is stable).
- H7 [pre-existing] 16 bits/key with 8 pattern bits: FPR ~1e-3 is likely over-provisioned for a pre-filter;
  8 bits/key (~2-3% FPR) halves array bytes => halves merge/replicate traffic and zeroing, with slightly
  more join input. Worth a microbenchmark (insert, OR-merge, probe throughput vs bits/key) before touching.
- H8 [pre-existing] No selectivity judgement at construction; domain gate needs a uniqueness proof. A cheap
  build-rows / probe-domain estimate (already computed: `estimated_selectivity_of`, `publisher.cpp:200-206`)
  is only attached as a hint, never used to skip construction.
- H9 [2570] Cap refusal drops all Bloom keys of a join together; not reachable under specific_config (16 GiB).

Probe efficiency (pre-existing, outside the stack's diff but in the DF hot path):
- H10 Ordinary path materializes a 1 byte/row BOOL8 mask per filter then compacts; fused path only covers
  signed i32/i64. Check ncu on `membership_probe_functor` for Bloom > L2 (expect DRAM-latency bound, low
  IPC); candidates: prefetch-free batching is not applicable on GPU, but ordering filters most-selective
  first and using the prior mask already exists - verify it is hit on TPC-H via the strategy counters.
- H11 Decode-time probe attachment happens at preparation; prefetched conversions decoded before
  publication lose in-decode filtering (endpoint still filters). With compressed pins this could matter
  for the first batches of each probe scan; quantify from `[dynamic_filter]` cached_* counters / Quent.

Timing:
- H12 [2570] Single-partition publication moved earlier (before CONCAT). Expect equal or better coverage;
  the regression risk is latency (H2), not lateness.

## 6. Profiling hooks available

- NVTX: `dynfilter::construction::begin`, `dynfilter::accum::contribute`, `dynfilter::stage::contribute`,
  `dynfilter::bounds::contribute`, `dynfilter::accum::finish`, `dynfilter::accum::late_wait`,
  `dynfilter::stage::gather`, `dynfilter::bounds::finish`, `dynfilter::replicate_devices`, marks
  `dynfilter::accum::final_commit` / `dynfilter::accum::visible`, `dynfilter::try_fused_membership_mask`,
  `dynfilter::apply_output`, `sirius_physical_dynamic_filter::execute`, `sirius_physical_build_filter::execute`.
- Logs: INFO `construction started: K key(s), B batch(es) on G GPU(s)`, `constructed filters published
  [(Bloom through HOST staging)]: ... ms after the final contribution`, `Bloom filters refused`;
  DEBUG `[dynamic_filter_key] ... kind= size= pushed= reason=`, `[dynamic_filter_domain]`,
  `[accumulated_bloom_builder::finish] ... bytes ... sources ... targets`.
- Counters: `SiriusContext::get_dynamic_filter_stats_snapshot()` (`src/op/dynamic_filter/dynamic_filter_stats.hpp`).
