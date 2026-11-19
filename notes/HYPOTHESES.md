# DF stack (#2569 -> #2570): ranked performance hypotheses + measurement plan

Synthesis of `construction.md`, `kernels.md`, `probe_publish.md`, `concurrency.md`, `pr_split.md`,
`tooling.md`, spot-checked against the code at `pr2570` (`ae939824`), base `c4fa3638`.
Read-only phase: **nothing here is measured**. Every impact is an estimate; the ranking is the order
in which to measure, not a result. Paths relative to `/opt/dlami/nvme/dfperf/repo`.

Owner tags: **[69]** = #2569, **[70]** = #2570, **[split]** = API/kernel half in 69 + call-site half
in 70, **[pre]** = already on main (not in the stack's diff; land separately, evaluate at the tip).

## 0. Spot-check results (corrections to the mapper notes)

Checked and confirmed in code:
- Forward path hooks `src/op/sirius_physical_partition.cpp:158-198`; hash path contribute before
  scatter `:229-232`, publish after scatter and before the task returns `:246`. The whole 2570 diff
  of this file is 18 lines.
- `contribute` (`src/op/dynamic_filter/dynamic_filter_publisher.cpp:1095-1171`): no CUDA under the
  session mutex; a non-GPU-tier batch or failed `reserve` ends the attempt (`:1136-1146`).
- `build_filter_construction::contribute` runs Bloom, stager, bounds in series on the task stream
  (`src/op/dynamic_filter/build_filter_construction.cpp:270-301`); `finish` is serial and ends in
  `root.stream.sync()` (`:305-400`).
- Bloom add is always `add_if_async` with a validity stencil, one kernel per key per batch, on the
  task stream (`src/cuda/sirius_dynamic_bloom_filter.cu:264-303`, `:1080-1121`). cuco is now fetched
  in the dfperf clone (`build/release/_deps/cuco-src`): `add_if_n` -> `add_coop`, as kernels.md says.
- Full-size lease + `cudaMemsetAsync` on every replica GPU (`src/cuda/accumulation_partial.cuh:167-209`);
  stager passes `zero_fill=true` (`src/cuda/exact_key_stager.cu:120`).
- Bloom sizing: 16 bits per valid row (`src/op/dynamic_filter/bloom_sizing.hpp:33-45`,
  `construction_plan.cpp:70-75`); domain cardinality is known for some keys
  (`detail/domain_gate.hpp:48-52`) but is not used for sizing.
- Chunk clamp 2-8 MiB (`complete_build_inventory.hpp:259-263`); sources = contributing non-root GPUs,
  targets = all non-root GPUs (`sirius_dynamic_bloom_filter.cu:1167-1172`).
- Bloom budget `max(4*scan_task_batch_size, max_build_hash_table_bytes)` = 16 GB/GPU
  (`src/sirius_config.hpp:271-274`). The "256 MiB cap refuses q3" finding is stale.

Corrections:
- **C1. Forward-path reservation did not go from 0 to 2x input.** `no_history_peak_memory_estimate`
  returned 0 at base, but the task then took the generic `2 x input` fallback
  (`src/pipeline/gpu_pipeline_task.cpp:977-986`). Net change at tip is `+scratch` only.
  `probe_publish.md` H4 and the `pr_split.md` lead overstate this; `construction.md` is right.
- **C2. The INFO line "N ms after the final contribution" is not the publication cost on the hash
  path.** `final_commit` is stamped in `contribute` (`publisher.cpp:1158`), before the final batch's
  `hash_partition`; `visible` is stamped after the scatter and `finish`. On the hash path the number
  includes one scatter. Use the NVTX ranges (`dynfilter::accum::finish`, `stage::gather`,
  `bounds::finish`, `replicate_devices`) for the pure cost. Same caveat for
  `accumulation_publication_latency_ns`.
- **C3. 12 bits/key does not compile as is**: `static_assert(256 % bloom_target_bits_per_key == 0)`
  (`bloom_sizing.hpp:37`). Only 8 / 16 / 32 work without relaxing the assert and `bloom_blocks_for`.
- **C4. P2P exists between GPU 0-1 and 2-3** (tooling.md). The transport choice is all-or-nothing
  (`find_pair_without_pool_access`, `sirius_dynamic_bloom_filter.cu:1191`), so 4-GPU runs are
  HOST_STAGED. A 2-GPU microbenchmark on `CUDA_VISIBLE_DEVICES=0,1` takes the PEER path: that is how
  to measure finish mechanics without the excluded host bounce.
- **C5. Arrival accepts GPU, host and disk tables** (`complete_build_inventory.cpp:189-213`), and
  declines only compressed or unreadable ones. But the ledger records only when the port barrier is
  FULL (`build_input_certifier.cpp:36-41`), and a contribution still needs a GPU-tier batch.
- **C6. NVTX**: `specific_config.yaml` has no `enable_nvtx`; that flag only gates the Quent NVTX
  injector (`src/telemetry/telemetry_context.cpp:41-46`, `nvtx_injection.cpp:73-78`). nsys sets
  `NVTX_INJECTION64_PATH` itself, so `dynfilter::*` ranges should appear. Unverified: check in the
  first nsys capture before relying on range attribution.

## 1. Excluded (user handles elsewhere) - report, do not optimize, do not attribute to kernels

- HOST_STAGED Bloom loop: sync after every leg, ~(2S+1+T) syncs and (2S+1+T) x bytes per chunk
  (`sirius_dynamic_bloom_filter.cu:1255-1325`).
- All-or-nothing transport choice (one missing pair sends every pair through HOST).
- HOST-staged exact/zone replica copies (`src/cuda/dynamic_filter_replica_transfer.cu:40-62`).

Consequence for evaluation: anything that cuts Bloom bytes or contributing GPUs also cuts host-bounce
time, so full-suite gains on this machine will be larger than on a P2P machine. Report every
finish-related result twice: full suite (this machine) and PEER-path microbenchmark (GPUs 0,1).

## 2. Ranked hypotheses

Impact = expected end-to-end effect on TPC-H SF1000, 4 GPUs. Risk = correctness/regression risk of
the candidate fix.

| ID | Claim (short) | Owner | Impact | Risk |
|---|---|---|---|---|
| H01 | Attempts declined or skipped at tip lose filters the one-shot path built | 70 (lease: 69) | H if any hit, else 0 | M |
| H02 | Bloom arrays are oversized (16 bits/row, rows not distinct keys): every multi-GPU byte cost scales with it | 69 sizing; constant pre | M-H | M |
| H03 | Single-partition / broadcast builds pay a G-way lease + merge + sync chain the one-shot path did not | 70 (split) | M | M |
| H04 | Bloom insert is a random scatter on GB arrays, run serially ahead of the scatter on the task stream | 69 | M | M |
| H05 | Filters the consumer gate rejects are still built, merged and replicated | 69 plan; policy pre | M | M |
| H06 | Publication is a serial chain of 4-6 host waits in the last PARTITION task, before its deposit | split | M-L | L |
| H07 | Eager full-size unspillable leases on every GPU from `begin` to finalize | 69 | L time, M memory | M |
| H08 | Many-small-batch builds: per-batch API overhead and one cross-GPU copy per staged slice | 69 (mutex part 70) | L-M | L |
| H09 | Hash path publishes after the final scatter | 70 | L | L |
| H10 | Publishing Task scratch is not reserved once history exists; root filters can be dropped | 70 | L (H if counter > 0) | L |
| H11 | A dead attempt keeps all its storage until site finalize | split | L | L |
| H12 | Transfer chunks of 2-8 MiB: hundreds of chunks per GB-scale array | 69 | L (PEER) | L |
| H13 | Bounds: Min and Max are two reductions; STRING bounds sync twice per batch | 69 | none on TPC-H | L |
| H14 | `_decision_mutex` held across all of `begin` (leases on 4 GPUs) | split | L | L |

### H01 - Declined / skipped attempts (coverage parity)  [70; lease refusal 69]
- Claim: construction is all-or-nothing per join. Any of these ends it, and the join then filters
  nothing, where the base one-shot path built from the concatenated batch regardless:
  compressed arrival, unreadable arrival (`try_to_read_only` fails), non-FULL port barrier,
  port directive / ORDER BY producer, lease refused at `begin`, batch not on a GPU tier at
  contribution, GPU without a lease, root IN-list refused by `build_within_room`.
- Code: `complete_build_inventory.cpp:189-213`, `build_input_certification.cpp:32-37`,
  `build_input_certifier.cpp:36-58`, `dynamic_filter_publisher.cpp:1136-1152`,
  `accumulation_partial.cuh:190`, `build_filter_construction.cpp:342-352`.
- Why first: one lost selective filter costs more than every kernel tweak below combined.
  `specific_config.yaml` has pin-table compression on, so compressed arrivals are plausible.
- Measure: one DEBUG-log pass per query at base, pr2569 and pr2570. Diff `[dynamic_filter_key]`
  lines by `sig/occ` (kind, size, pushed, reason). Grep `constructs no dynamic filter`,
  `construction skipped`, `construction declined`. Counters `accumulations_declined_*`,
  `accumulations_skipped_{inventory,admission,error,transient}`,
  `filters_skipped_construction_admission`. All 22 queries; q20 is the known differing one.
- Candidate fix (only for reasons that actually occur): take arrival facts from compressed
  metadata; per-key rather than per-join failure; retry lease at smaller bits/key.
- Also record the other direction: filters tip publishes that base did not (exact lists and
  non-integer keys on multi-partition builds). Feeds H05.

### H02 - Bloom bytes  [69 `construction_plan.cpp` `bloom_for`; constant in `bloom_sizing.hpp` is pre]
- Claim: array bytes B = 2 x valid rows. On G GPUs B drives memset (G x B), ingress (S x B),
  OR, egress ((G-1) x B), unspillable memory (G x B), and insert/probe cache behaviour.
  Two separate levers:
  - a. **Distinct-key cap**: size on `min(valid_rows, domain.cardinality)` when the domain is known.
    Duplicate-heavy builds (counter `keys_build_exceeded_domain`) are oversized. No quality loss.
  - b. **Bits per key**: 16 -> 8 halves B. Estimated FPR goes from ~0.1-0.3% to ~3-5% (8 pattern
    bits, one per word). Could be size-adaptive (keep 16 while B fits L2 or a few hundred MB).
- Measure: (a) log pass: per Bloom key `rows`, `valid`, domain cardinality -> bytes saved.
  (b) microbench MB1/MB6 (insert + probe throughput and measured FPR at 8 vs 16) and MB4 (finish vs
  bytes); then full suite at 8 vs 16 on the Bloom-heavy queries (expected q3, q5, q7, q8, q9, q10,
  q18, q21; confirm from the log pass).
- Risk: (a) low if the domain bound is a true upper bound on distinct keys (check provenance).
  (b) more join input from false positives; needs the full suite to judge.

### H03 - Forward path vs the one-shot build  [70; any fast path inside the accumulators is 69]
- Claim: base built single-partition / broadcast filters with one kernel over the concatenated
  batch on one GPU, then copied to the other GPUs. Tip, per join: up to 3 x G stream creates,
  G leases + memsets, one kernel set per batch, and at publication S full-array ingress legs (the
  batches sit on the GPUs that scanned them), 4-6 host waits, per-filter replicate sync.
  For small builds this is fixed cost per join, and several queries have many small joins.
- Code: `sirius_physical_partition.cpp:158-198`, `build_filter_construction.cpp:104-240, 305-400`,
  `dynamic_filter_publisher.cpp:650-665`.
- Measure: full suite with nsys on small-dimension queries (expected q2, q5, q7, q8, q9, q11, q16,
  q17, q19, q20): sum of `dynfilter::construction::begin` + contribute ranges + finish ranges +
  `replicate_devices` per join at tip, against `dynfilter::push_build_side` at base. Quent: task
  time of forward-layout build PARTITIONs, base vs tip. Microbench MB5.
- Candidate fix: when the certified inventory sits on one GPU, lease only there and replicate at
  publication (the base shape); skip stream/lease creation for accumulators with no keys; one wait
  for the whole finish (H06).
- Offsetting benefit to quantify, not assume: publication now precedes CONCAT, so transitive and
  racing consumers can see the filter earlier.

### H04 - Bloom insert at PARTITION  [69]
- Claim: one 32-byte random read-modify-write per row into an array far larger than L2 (300 MB to
  3 GB+). On the hash path it is queued on the task stream ahead of `cudf::hash_partition`, whose
  internal host syncs then wait for it: task time = insert + scatter. On the forward path it is the
  task's only GPU work. The kernel itself already conforms to the best-practices guide (blocked
  layout, one sector per key, launch bounds), so there is no cheap kernel-only fix.
- Code: `sirius_dynamic_bloom_filter.cu:264-303, 1080-1121`, `build_filter_construction.cpp:294`.
- Measure: MB1 under ncu (`add_if_n`: memory throughput, L2 hit rate) for arrays 16 MB / 300 MB /
  1.5 GB / 3 GB / 12 GB. Full suite: nsys kernel time of `add_if_n` inside
  `dynfilter::accum::contribute`, as a share of `sirius_physical_partition::execute`.
- Candidates, in order of cost:
  1. Overlap: queue the insert on a second stream so it runs beside the scatter (the event fold
     already orders later readers). Risk: the input must outlive the kernel; tracker accounting.
  2. `add_async` without stencil when the batch has no nulls; `ConditionalAdd` for duplicate-heavy
     keys. Both are MB1 variants; expected small.
  3. Fewer bytes (H02) improves TLB/L2 behaviour.
  4. Locality (bucket by block, or partition-clustered inserts after the scatter): only if ncu shows
     the kernel is memory-latency bound and its share is material. Changes the probe hash. High risk.
- Not a saving: sharing the partition hash (32-bit Murmur3 inside cuDF vs 64-bit xxhash).

### H05 - Filters built but not useful  [69 plan policy; consumer gate is pre]
- Claim: construction never judges selectivity. A Bloom the consumer disables after one sampled
  batch (keep > 0.9), or skips per filter (keep > 0.5), was still inserted, merged and replicated.
  Tip widens exposure: every key type on multi-partition builds [69], every layout [70], and one
  independent array per key column on multi-key joins.
- Code: `construction_plan.cpp:119-196`; hint only at `dynamic_filter_publisher.cpp:683-688`;
  gate `src/op/scan/dynamic_filter_gate.hpp`.
- Measure: per query, join the `[dynamic_filter_key]` lines (bytes built) with the consumer gate
  outcome per filter. A/B `SET enable_dynamic_filter = false` (SQL, not the config file) per query
  gives the net value of DF; queries with net value <= 0 are the targets.
- Candidate fix: skip at plan time when `estimated_selectivity_of` predicts keep above the
  consumer threshold; on multi-key joins skip the weaker key.
- Risk: a wrong estimate loses a useful filter.

### H06 - Serial finish chain  [split: accumulators 69; replicate loop and call site 70]
- Claim: in order, each with its own host wait: bounds readback (`bounds_accumulator.cu:439`, +`:518`),
  Bloom merge wait (`late_wait`, `:874`), stager per-source sync + final sync
  (`exact_key_stager.cu:279, 316`), root filter build + `root.stream.sync()`
  (`build_filter_construction.cpp:398`), then one sync per replicated exact filter / zone map
  (`sirius_dynamic_in_list_filter.cu:361-368`). All inside the Publishing Task, which holds its
  reservation, input and output and has not deposited yet.
- Measure: MB4 at G=1 (pure sync chain) and G=2 on GPUs 0,1. Full suite: per-join sum of the finish
  ranges; Quent duration of the last build-PARTITION task against the median task.
- Candidate fix: queue everything, one readback buffer (bounds slots + overflow words), one wait;
  replicate all exact filters in one fan-out with one wait per target.
- Expected: sub-millisecond to a few ms per join on the PEER path. Matters with many joins.

### H07 - Eager leases  [69]
- Claim: every accumulator leases full-size storage on every replica GPU at `begin`, when the whole
  build input is resident. Arrays on non-contributing GPUs are zeroed, then fully overwritten at
  egress. The memory is invisible to the downgrade executor; budget allows 16 GB/GPU per join.
  A refused lease skips the whole attempt (H01).
- Code: `accumulation_partial.cuh:167-209`, `build_filter_construction.cpp:104-240`.
- Measure: `dynfilter::construction::begin` range; MB3; Quent reservation waits / downgrades during
  build PARTITION on big-build queries; `accumulations_skipped_admission`.
- Candidate fix: lease on first contribution; lease pure targets at publication without memset.
- Risk: a late lease can fail after work was done (wasted inserts).

### H08 - Many small batches  [69; mutex section 70]
- Claim: a selective scan of a big table yields many batches with few rows each. Each pays, per
  accumulator: `cudaStreamGetDevice`, event create/record/destroy, two `cudaStreamWaitEvent`, plus
  two session-mutex sections with `batch_key_facts::of` over all columns under the lock. Exact
  keys add one cross-GPU replica copy per (batch, key) slice at gather (`exact_key_stager.cu:283`),
  a zero-fill of buffers that are fully overwritten (`:120`), and one 4-byte alloc/memset/D2H per
  (GPU, key) (`:117, 303-305`).
- Measure: log pass gives batches per construction (`construction started: K key(s), B batch(es)`).
  If B is routinely > ~50 with exact keys, run MB2 (tiny batches, CUDA API counts in nsys).
- Candidate fix: one shared partial stream + one event per contribution; coalesce adjacent slices
  per source GPU; zero only the overflow words, packed in one array; facts for planned keys only.

### H09 - Publish after the final scatter  [70 call site; ordering predates the stack]
- `sirius_physical_partition.cpp:229-246`. Filters could be visible one scatter earlier, or `finish`
  could be queued before the scatter and waited after it. Helps only consumers not held behind the
  build edge (transitive targets, `scans.front()`, STANDARD/MIXED joins). Measure: gap between
  `final_commit` and the start of `dynfilter::accum::finish` in nsys; whether any target scan is
  running in that window (Quent).

### H10 - Publishing Task scratch with history  [70]
- `sirius_physical_partition.cpp:506-518` adds scratch only in the no-history estimate. The
  Publishing Task is the last, so it always has history. Check
  `filters_skipped_construction_admission` and whether Bloom scratch OOM forced HOST staging.
  Fix: add scratch on top of the history estimate for Construction Site tasks.

### H11 - Dead attempt retention  [69 lifetime; 70 call site]
- After `end_attempt`, storage is held until `finalize_input` (`dynamic_filter_publisher.cpp:840-852`).
  Only matters if H01 counters are nonzero on big builds. Fix: release at once.

### H12 - Chunk size  [69]
- `complete_build_inventory.hpp:259-263`. 3 GB = 384 chunks x (S+T copies + 1 kernel + events).
  Try 32-64 MiB in MB4 on the PEER pair. Larger chunks also cut HOST-path syncs, but that gain is
  the excluded part: report it separately.

### H13 - Bounds  [69]
- Min and Max as two CUB reductions + one temp alloc per key per batch
  (`bounds_accumulator.cu:131-144`); STRING bounds sync twice per batch (`:386-392`). Zone maps are
  off under this config, so only DECIMAL128 keys reach it. Verify that no
  `dynfilter::bounds::contribute` range appears in TPC-H; if none, do not spend time here.

### H14 - `_decision_mutex` across `begin`  [70 certifier; 69 begin body]
- `build_input_certifier.cpp:43-66`. One-off per join on a task-creator thread. Covered by the
  `dynfilter::construction::begin` range; act only if it shows tens of ms.

Dropped after review: sharing the partition hash; lock contention on the session mutex (never held
across CUDA work); O(batches^2) paths (none found); Bloom cap refusal (not reachable at 16 GB).

## 3. Measurement plan

### 3.1 Baseline full-suite profile (before any prototype)
Environment block and commands: `tooling.md` sections 0, 2-5. One GPU process at a time. Never edit
`specific_config.yaml`. Everything on NVMe (`--output $R/runs`, `TMPDIR`, `SIMPATICO_JIT_CACHE_DIR`).
Open point: Quent writes to the root disk (11 GB free) on every run; needs the user's call.

1. **Log pass (not timed)**, tip, DEBUG, 1 iteration, all 22 queries. Output: one table per query
   with every construction: join id, layout (single / multi / broadcast, from `partition strategy:`),
   keys, kind, bytes, batches, GPUs, decline/skip reason, HOST-staged flag. This selects the queries
   for steps 3-4 and settles H01, H02a, H05 (bytes built), H08 (batch counts), H13.
2. **Timing**, tip: fresh process, 1 warm + 3 hot iterations, median; A/A noise per query from a
   repeated block. Then the same with `SET enable_dynamic_filter = false` (pre-SQL) for the net
   value of DF per query. Optional but recommended: a base build (`c4fa3638`) for the key-line diff
   (H01) and the one-shot reference (H03); pr2569 timing adds little because its new code is dormant.
3. **Quent** (emitted by step 2): per build PARTITION, task count, median and last-task duration
   (Publishing Task excess = publication cost), forward-layout PARTITION task time, reservation
   waits and downgrades during build PARTITION. There are no DF-specific Quent events.
4. **nsys**, one query per capture, for the queries step 1 flags (largest Bloom bytes, most
   constructions, forward-path builds, any decline): NVTX range sums for `dynfilter::*`, kernels
   inside them (`add_if_n`, memset, CUB/thrust), `cudaStreamSynchronize` time inside finish ranges,
   and memcpy bytes split into HOST-staged (excluded, reported) and the rest.
5. **Gate**: pursue a hypothesis only if its measured share is above the A/A noise of the query
   (rule of thumb: >= 1% of a query, or >= 0.5% of the suite total).

### 3.2 Microbenchmarks to build
Catch2 `[!benchmark][dynamic_filter]` file in `sirius_unittest` (pattern
`test/cpp/scan/bench_decode_codecs.cpp`, fixture `acc::fixture`, API `plan_construction` ->
`try_create` -> `reserve`/`contribute` -> `finish`). Benchmark files are prototype-only unless the
user wants them landed; if landed, MB1-MB4 and MB7 belong with [69], MB5 with [70], MB6 separate.

| ID | What | Sweep | Tool | Serves |
|---|---|---|---|---|
| MB1 | Bloom insert throughput | array 16 MB / 300 MB / 1.5 GB / 3 GB / 12 GB; batch 1M / 16M / 128M rows; int32 / int64; `add_if` vs `add`; ConditionalAdd; dup x1 / x10; 8 vs 16 bits/key; alone vs with `cudf::hash_partition` on the same stream vs a second stream | nsys + ncu `--set full` on `add_if_n` | H04, H02 |
| MB2 | Per-contribution fixed cost | 1 to 1K rows; 1 / 2 / 4 keys; Bloom vs exact vs bounds; 1 to 512 batches | nsys CUDA API trace | H08 |
| MB3 | `begin` cost | bytes 1 MB to 12 GB; G = 1 / 2 / 4 | nsys | H07, H14 |
| MB4 | `finish` cost | G = 1 (sync chain), G = 2 on GPUs 0,1 (PEER), G = 4 (HOST, report only); bytes; chunk 8 / 32 / 64 MiB; exact slices 1 / 64 / 512 | nsys | H06, H12, H02 |
| MB5 | Small build end to end vs one-shot shape | rows 12 / 1e3 / 1e5 / 1e7; batches 1 / 16 / 128; G = 1 / 4; against prepared-keys ctor + `replicate_to_devices` | nsys | H03 |
| MB6 | Probe throughput and measured FPR | 8 vs 16 bits/key; arrays 300 MB to 3 GB; hit rate 1% / 10% / 50% | ncu + nsys | H02b |
| MB7 | Bounds Min+Max vs fused | 1M / 10M / 100M rows | nsys | H13, only if TPC-H reaches it |

## 4. PR ledger (where each fix would land)

- **#2569**: H02a (distinct-key cap in `bloom_for`), H02b if adopted as a sizing policy, H04
  (stream placement, add variants), H05 (plan-time skip), H06 accumulator half (single readback /
  single wait), H07, H08 (shared partial + event, slice coalescing, zero-fill), H11 lifetime, H12,
  H13. TPC-H effect is visible only at the pr2570 tip; at pr2569 prove with unit tests + microbench.
- **#2570**: H01 (decline policy, certifier), H03 (forward-path fast path, single-GPU inventory
  shape), H06 replicate loop, H09, H10, H14 mutex scope, H08 mutex section.
- **Pre-existing, outside the stack**: `bloom_target_bits_per_key` constant, consumer gate,
  probe kernels, Bloom star reduce / `chunk_pipeline`, HOST staging. Evaluate at the tip, land
  separately.
- Rule for split changes: the 69 half must compile and pass tests against the old session and use
  no 70 symbol; never optimize code 70 deletes (one-shot path, shared geometry).
