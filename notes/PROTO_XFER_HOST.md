# PROTO_XFER_HOST: doing the unavoidable host bounce well (DF finish chain) - prototype + measurements

2026-11-19. Worktree `/opt/dlami/nvme/dfperf/wt-xfer-host`, branch `proto/xfer-host` on top of pr2570 `ae939824`
(+ cherry-pick of the lifecycle microbench `a5b37728`). Local commits only, nothing pushed.
Scripts `$R/runs/xfer_host/bin/`, raw output `$R/runs/xfer_host/`, Quent `$R/telemetry/xfer-host*`.
Every build and GPU process ran under `flock $R/machine.lock` (one queued command per hold, `bin/next.sh`).
Config `/home/ubuntu/sirius/specific_config.yaml` untouched; the prototype is switched by environment variables only.

STATUS (2026-11-19, second session after the wipe): BUILT, gates pass, measured. Sections 1-2 are design (code facts, written
before the first build; still accurate), 3+ are measurements of this session. Section 9 lists what ran and what did not.
Commits on `proto/xfer-host`: `a57417f7` (mb/lifecycle cherry-pick), `336915e1` (design as recovered), `ff814aec` (compile fix +
byte dump for the identity test), `e683d765` (exact-kind identity test). Pushed to the fork as `dfperf/proto/xfer-host` by backup.sh.

## 1. Scope and seam

Item: keep today's star topology (sources -> root, root -> everyone) and today's all-or-nothing transport choice
(`find_pair_without_pool_access`), and rebuild only what happens once that choice says HOST. The per-pair D2D and the
topology-aware reduce are another agent's prototype (`runs/xfer_d2d`).

Seam (how the two compose):
- The choice of *transport per leg* and the choice of *schedule* are separate in the prototype. `host_pipeline`
  (`sirius_dynamic_bloom_filter.cu`) describes a publication as legs: download(source chunk -> HOST slot),
  upload(HOST slot -> GPU chunk), each on a named stream, ordered by events. A P2P leg is the same leg with the two
  halves replaced by one `cudaMemcpyPeerAsync` on the destination stream and no slot. So a per-edge transport
  ("this edge has pool access -> peer copy, else bounce") slots into `copy()` / `upload_to()` without changing the
  event graph: the slot-reuse events just become unused for peer legs.
- The reduce topology (which GPU ORs what) is the `schedule` enum (`fanout`, `gpu_or`, `host_or`). A pair-local P2P
  pre-merge (the other agent's `c2` / `pairhostor_pipe`) changes only the *inputs* of the cross-pair stage: after it,
  each pair has one holder, and the cross-pair stage is exactly this prototype's `host_or` (or `gpu_or`) over 2
  inputs instead of 4, followed by a fan-out whose in-pair legs are P2P.
- `finish_impl` picks: PEER pipeline if every pair has access (unchanged), else `host_pipeline` if the switch is on
  and its pinned window can be allocated, else today's loop (unchanged, the fallback).
- Replica copies (`enqueue_device_copy`) already choose the route per (source, destination) pair; the prototype only
  changes what the HOST route does (`host_bounce_scope`), so it composes with per-pair D2D by construction.

## 2. What the prototype changes (all behind switches; switch off = tip code path, byte for byte)

| Switch | Values | Effect |
|---|---|---|
| `SIRIUS_PROTO_XFER_HOST` | unset / `off` | tip loop |
| | `gpuor` | star kept, OR on the root GPU, every leg pipelined through pinned HOST slots |
| | `hostor` | OR on the CPU: contributors download, CPU ORs, every GPU uploads |
| | `auto` | `hostor` with >= 2 non-root sources, else `gpuor` |
| `SIRIUS_PROTO_XFER_WINDOW` | N (default 4) | HOST slots in flight per array (bounds the pinned bytes) |
| `SIRIUS_PROTO_XFER_OR_THREADS` | N (default 16) | CPU threads of the host OR |
| `SIRIUS_PROTO_XFER_REPLICA` | 0 / 1 (default: on iff the first switch is on) | stage-once replica fan-out + batched exact-key gather |
| `SIRIUS_PROTO_XFER_FAIL_HOST` | 1 | test: pretend the pinned window cannot be allocated (exercises the fallback) |
| `SIRIUS_PROTO_XFER_CHECKSUM` | 1 | test: checksum every replica array inside `finish` |

### 2.1 Bloom publication, HOST transport (`host_pipeline`, replaces the loop at tip `:1255-1325`)

Today per chunk (2-8 MiB): for each source `D2H, sync, H2D, sync, OR`; then `D2H, sync`; T uploads, T syncs. One 1-chunk
HOST window, reused; 2S+1+T host waits per chunk; every leg waits for the previous one.

Prototype, three schedules, same chunk size (so the device scratch stays inside the task's existing reservation):

- `fanout` (at most one GPU contributed: it already holds the union; today's S=0 case and the "one non-root source,
  root empty" case): per chunk one download into a HOST slot on the holder's partial stream, then one upload per
  other GPU from that single slot, each on that GPU's own upload stream. Pure event ordering, 0 host waits until the
  final one. Bytes: B up, (G-1) x B down (same as today for S=0; one download fewer than today when the holder is not
  the root).
- `gpu_or` (star): every source downloads its chunk into its own slot on its own partial stream (in parallel across
  GPUs); the root uploads the S slots into device scratch on the publication stream, ORs them on its partial stream
  (same kernel), downloads the merged chunk once into an output slot; every target uploads from that slot on its own
  stream. Event ordered end to end; 1 host wait. Bytes: (S+1) x B up, (S + G-1) x B down (same 2S+1+T legs as today,
  but overlapped).
- `host_or`: every contributing GPU (root included) downloads its chunk (window of W chunks in flight per GPU); the
  host waits for chunk k's downloads (`cudaEventSynchronize`), ORs the slots on the CPU (AVX-512, non-temporal
  stores, a small persistent thread pool), and queues G uploads from the output slot. Bytes: C x B up, G x B down
  (C = contributors). No device scratch at all. The root's link carries B each way instead of S x B in + B out.

Streams: downloads of a GPU run on its partial stream (they follow its inserts with no extra event); uploads run on a
second stream per GPU (owned by the pipeline; the root uses the publication stream), so a GPU's two link directions
overlap and an upload blocked on an event never blocks that GPU's downloads. At the end every upload stream is folded
into its partial stream (later frees follow it) and into the publication stream: one `cudaStreamSynchronize`.

Slots: 1 MiB blocks of the engine's fixed-block pinned HOST pool (`allocate_multiple_blocks`, same resource and same
no-reservation call as today), one batch memcpy per leg (`device_copy_batch`, entries never fused, as today).
A slot is reused only after its readers' events (uploads of chunk k-W for output slots; the root's ingress for
`gpu_or` input slots; the host OR itself for `host_or` input slots).

### 2.2 Replica copies and exact-key gather

- `host_bounce_scope` (`device_transfer.{hpp,cpp}`): optional argument of `enqueue_device_copy` /
  `enqueue_replica_copy`. With it the HOST route downloads a source extent once (async, on a source-GPU stream,
  event recorded) and each destination uploads from that copy on its own stream after the event; no host wait. A
  second destination of the same extent reuses the staged bytes (stage once, fan out). The scope owns blocks and
  source streams and drains everything in its destructor. Used by the three replica fan-outs (hash IN-list set,
  small IN-list needles, one-shot Bloom); their existing final per-target sync is the only wait.
- Exact-key gather (`exact_key_stager::gather`): for a source GPU without pool access to the root, all its written
  slices go in ONE download batch on the source's own partial stream (no host sync for "contributions complete": the
  stream order gives it), packed back to back in pinned blocks, one event, ONE upload batch on the publication stream.
  Today: a host sync per source, then per slice `D2H, sync, H2D, sync` with >= 1 MiB of staging each.

### 2.3 Memory and failure paths

- Pinned HOST bytes held for the duration of one finish: `fanout` W x chunk; `gpu_or` (S+1) x W x chunk;
  `host_or` (C+1) x W x chunk (chunk <= 8 MiB; W = 4, 4 GPUs all contributing: 160 MiB; today: 8 MiB).
  Measured values in section 4.
- Device scratch: `gpu_or` min(2, chunks) x S x chunk = the PEER pipeline's scratch, which
  `publication_scratch_bytes` already reserves; `fanout` / `host_or` none (today's HOST loop: 1 chunk).
- Pinned window or scratch not available (`std::bad_alloc` / `rmm::out_of_memory`): nothing has been enqueued yet;
  the code syncs the publication stream, drops what it got and runs today's loop (1 chunk). Exercised by
  `SIRIUS_PROTO_XFER_FAIL_HOST=1` in the correctness test. Gather: falls back to the per-slice path.
- A failure after enqueueing: the pipeline's upload streams are added to the `joined` list, so `join_on_failure`
  drains them like the partial streams; if a stream cannot be joined the pipeline object (pinned blocks, streams) is
  leaked like the tip leaks `staging` / `scratch`.
- NUMA: this host is one NUMA node (MB_TRANSFER section 1), so placement of the staging blocks is not testable here.
  The slots come from `root->host_space` like today.

## 3. Correctness gates

| Gate | What | Result |
|---|---|---|
| Bloom bit identity, G=4 (`[xfer_proto]`, `out/check_g4.log`) | 5 array shapes (96 B .. 70 MiB, ragged tails, 2 keys) x 6 contribution sets (root only, non-root only, 2, 2 non-root, all, none) x 2 roots x 9 configs (gpuor w1/2/4, hostor w1/4, hostor w3 t1, hostor w8 t5, gpuor+forced fallback, hostor+forced fallback). Every array of every GPU downloaded and `memcmp`'d against the tip loop's (force-host) arrays, plus a position-dependent 64-bit checksum | 540 finishes, 2592 arrays, **all identical** |
| Exact-kind identity, G=4 (`[xfer_proto_exact]`, `out/exact_g4.log`) | sessions (`publish_if_final`) over 12 / 1920 / 30720 / 199680 keys, 1-2 key columns, 1 / 6 / 8 / 64 batches, on GPU 0 or round-robin; every published IN-list probed on every GPU with the dense range [-8, max+8); replica switch 0 vs 1 vs 1 + forced HOST-allocation failure | 96 sessions, 576 masks (hash IN-list 42 cases, small IN-list 6), **identical and equal to the inserted key set**; proto paths ran: 144 staged sources, 144 stage-once hits, 40 batched gathers |
| Query results | `result.txt` md5 of every process of every arm | identical for every query run (section 5) |
| Fallback (pinned window refused) | `SIRIUS_PROTO_XFER_FAIL_HOST=1` inside both gates | tip loop / per-slice gather taken, arrays and masks identical |
| Proto binary, switch off | arm O (q21, q3) | q21 -1.9 ms, q3 -7.5 ms vs tip binary (noise) |
| Existing DF unit tests | section 8 | see there |

Not covered by a gate: one-shot Bloom `replicate_to_devices` with the stage-once scope (only reached when a Bloom is built
on one GPU outside the accumulation path; exercised by the unit tests of section 8, not byte-compared here).

## 4. Lifecycle microbenchmark MB4P (builder-level `finish` wall, G=4, root GPU 0, 5 reps, median; min within 0.3%)

Batches on 1 GPU = root only (S=0, fan-out); 2 = GPUs 0,1 (one source); 4 = all (S=3). `off` = tip loop.
ms (ms per GiB of Bloom). Checksums of all 4 arrays agree with the tip loop in every row (`verified=1`).

| Bloom | batches on | tip loop | gpuor w4 | hostor w4 t16 | best vs tip |
|---|---|---|---|---|---|
| 57 MiB | 1 | 3.43 (62) | 2.37 (42) fan-out | same | 1.45x |
| 57 MiB | 2 | 5.79 (104) | 3.05 (55) | 3.08 (55) | 1.90x |
| 57 MiB | 4 | 10.45 (188) | 5.59 (100) | **3.22 (58)** | 3.25x |
| 174 MiB | 1 | 10.37 (61) | 6.79 (40) fan-out | same | 1.53x |
| 174 MiB | 2 | 17.54 (103) | 8.28 (49) | 8.45 (50) | 2.12x |
| 174 MiB | 4 | 31.59 (186) | 16.37 (96) | **8.21 (48)** | 3.85x |
| 281 MiB | 1 | 16.75 (61) | 10.81 (39) fan-out | same | 1.55x |
| 281 MiB | 2 | 28.42 (104) | 12.93 (47) | 13.27 (48) | 2.20x |
| 281 MiB | 4 | 51.02 (186) | 26.33 (96) | **12.95 (47)** | 3.94x |
| 1434 MiB | 1 | 85.30 (61) | 54.18 (39) fan-out | same | 1.57x |
| 1434 MiB | 2 | 145.00 (104) | 62.40 (45) | 64.95 (46) | 2.32x |
| 1434 MiB | 4 | 259.82 (186) | 133.35 (95) | **62.72 (45)** | 4.14x |

- Tip reproduces MB4 / MB_TRANSFER exactly (61 / 104 / 186 ms/GiB).
- `hostor` with every GPU contributing: 45-58 ms/GiB. That is at the link bound of this machine for any schedule that keeps
  all legs on the host (each pair uplink carries 2 x B up and 2 x B down; duplex rate 41 GiB/s each way = 49 ms/GiB) and
  **better than the best topology-aware schedule of the primitives bench** (`c2` 52-59, `pairhostor_pipe` 52-53 ms/GiB).
- `gpuor` (star, OR on the root GPU) is 1.9x only: the root's port takes 3 x B in and B out. It ties `hostor` with one
  source. `auto` = `hostor` with >= 2 sources, else `gpuor`; a single contributor is a pure fan-out (39-43 ms/GiB = the
  uplink of the other pair carrying two uploads).
- Pinned HOST held during one finish: fan-out 32 MiB, gpuor 64-128 MiB (+ 14-48 MiB device scratch on the root, the PEER
  pipeline's amount), hostor 96-160 MiB and **no device scratch** (tip: 8 MiB pinned + 8 MiB scratch).
- Host waits before the final one: 0 for fan-out and gpuor; hostor waits on events per chunk (it paces the CPU OR).

(Sweeps: window / OR threads / chunk size / small arrays / G=2: section 7.)

## 5. TPC-H SF1000, 4 GPUs: ABBA timing, tip binary (`$R/repo`) vs prototype with `SIRIUS_PROTO_XFER_HOST=auto`

Fresh process per query, 6 iterations (1 warm + 5 hot, first hot dropped), arms interleaved A U U A / U A A U inside one lock
hold per query (2 queries per hold later on). ms = median of the 8 kept hot iterations (2 processes x 4).

PLACEHOLDER_ABBA

## 6. nsys (q21, q3; 1 capture per arm, 1 warm + 2 hot; same-thread attribution, `runs/profile/bin/nsys_df.py`)

| q | arm | query wall ms (2 hot) | `accum::finish` wall ms (sum of 2 publications) | largest finish ms | host waits in finish | ms inside waits | copies | GiB copied | CPU OR ms (n chunks) |
|---|---|---|---|---|---|---|---|---|---|
| 21 | tip | 1479, 1449 | 293.3, 308.1 | 266.8, 281.6 | 1932 | 274, 289 | 1920 | 14.91 | - |
| 21 | auto | 1238, 1255 | 90.8, 74.2 | 81.1, 66.0 | 1522 | 45, 35 | 1536 | 11.93 | 25.4, 18.9 (192) |
| 3 | tip | 669, 662 | 62.2, 62.4 | 58.0, 58.3 | 404 | 58 | 392 | 2.96 | - |
| 3 | auto | 626, 622 | 22.0, 22.8 | 19.0, 19.8 | 290 | 12 | 320 | 2.41 | 5.6, 6.3 (36) |

- Finish wall: q21 300 -> 82 ms (3.6x), q3 62 -> 22 ms (2.8x). The saving equals the query saving (q21 -208, q3 -41 ms).
- Bytes: 10 x B -> 8 x B for S=3 (4 downloads + 4 uploads instead of 3 + 3 + 1 + 3); every byte still goes through the host.
  CUPTI's per-kind split (HtoD / DtoH / PtoP) is not usable for these batch copies (it labels most as PtoP in both arms, as
  noted in MB_TRANSFER section 8); the engine issues only host legs here, so "by transport" = 100% HOST in both arms.
- Host waits: tip 1932 `cudaStreamSynchronize`, all blocking for one leg each (274-289 ms inside them). Proto: 1522 calls,
  of which 1506 are `cudaEventSynchronize` of the hostor pacing (35-45 ms inside them: the host thread waits for a download
  only when the links, not the CPU, are the limit) + 2 final waits (1 ms).
- In the query the 1.39 GiB publication of q21 takes 66-81 ms (48-58 ms/GiB) against 62.7 ms in the microbench: the
  query has other work on the links and CPUs.

## 7. Sweeps (MB4P)

PLACEHOLDER_SWEEPS

## 8. Existing DF unit tests on the prototype binary

Filters as planned: single GPU `[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]`; 4 GPUs
`[dynamic_filter][multi_gpu],[device_transfer],[publisher],[construction_plan],[physical_partition],[build_filter]`.

| Arm | single GPU | 4 GPUs |
|---|---|---|
| `SIRIUS_PROTO_XFER_HOST=auto` (replica switch on with it) | 638 cases, 141567 assertions, all pass | 159 cases: 155 pass, **4 fail** (36 assertions) |
| switch off | PLACEHOLDER_UT_OFF |

The 4 failures under `auto` all pin the tip loop's workspace, none compares filter contents:
- `accumulated publication matches the complete partial union and bounded ...` (`test_dynamic_filter_accumulation.cpp:2131,2134`,
  32 assertions) and `HOST scratch uses the unused task reservation and returns its excess` (`:2208`): require
  `workspace.host_staging_bytes == 1 chunk` and `device_scratch_bytes == 1 chunk`. The prototype holds a window of
  chunks on the host and no device scratch (hostor / fan-out). The union checks of the same test pass.
- `optional publication scratch OOM deposits without rescheduling the PARTITION` (`test_in_task_publication.cpp:895`):
  requires exactly one device-scratch request; hostor makes none.
- `publication scratch allocation failure retires ordinary-pool partials` (`:953`): injects a failing scratch allocator and
  requires `finish` to return no publication. hostor needs no device scratch, so it **publishes** where the tip gives the
  filter up. A behaviour change in the good direction, but a product version must decide it and update the test.

## 9. What ran / what did not

Ran (all under `flock $R/machine.lock`, one queued job per hold, `runs/xfer_host/queue.log`): build (2 holds), `[xfer_proto]`
G=4, MB4P short + sweeps, ABBA on PLACEHOLDER_NQ queries, nsys q21 + q3 (2 arms), `[xfer_proto_exact]` G=4, DF unit tests
(arms: PLACEHOLDER_UT_ARMS), MB4x / MB2 exact-kind microbench.
Not run / unverified:
- `gpuor`-only and `hostor`-only arms in TPC-H (only `auto`); `SIRIUS_PROTO_XFER_REPLICA` alone in TPC-H.
- Unit-test arms `hostor` and `gpuor` (only `auto` and off). `[cuda_status_fault]` tests not run with the switch on:
  the failure paths after enqueueing (join of the upload streams, leak on an unjoinable stream) are **untested**.
- Contention with a concurrent heavy copy (MB_TRANSFER section 7 predicts hostor is the least sensitive schedule; not
  re-measured in the engine). Eager probe scheduling (S6) is not in this binary: the gain here is measured with the finish
  inside the Publishing Task at the tip's place.
- A machine with NUMA nodes, a full-P2P 4-GPU machine and a machine with one root port per GPU: models only (section 10).
- Lock contention was heavy (8-9 waiters, 10-20 min per turn), so queries without a HOST-staged Bloom got one A/U pair
  (4 hot per arm) instead of ABBA; marked in section 5.

## 10. Expected behaviour on other machines

- **Full P2P (every pair has pool access).** `finish_impl` takes the PEER `chunk_pipeline` before the prototype is
  consulted, and `enqueue_device_copy` takes the peer route: the prototype code is not reached. Zero gain, zero loss
  (PLACEHOLDER_G2PEER). The 20 ms/GiB per target of MB4 stays.
- **This machine (P2P inside pairs only).** Measured: finish 186 -> 45-58 ms/GiB with all GPUs contributing, -PLACEHOLDER_SUITE
  on the suite. It is already at the uplink bound for host-only legs; what the per-pair D2D prototype can still add is the
  difference between 2 x B and B on each pair uplink (bound 38 ms/GiB instead of 49; `c2` measured 52 in the primitives
  bench, i.e. no better than hostor as built here).
- **No P2P at all, one root port per GPU.** Today's loop is the same 186 ms/GiB (serial legs do not use parallel links).
  hostor needs B up + B down per GPU link in duplex (47 GiB/s each way): bound 21-22 ms/GiB, i.e. the PEER rate.
  PLACEHOLDER_G2HOST
- **No P2P, shared uplinks** (this machine with the pair P2P ignored) = exactly what was measured: the prototype never uses
  P2P when one pair lacks access.
- More GPUs: hostor is O(1) in host waits per chunk and its pinned window grows as (contributors + 1) x W x chunk; the CPU OR
  reads `contributors` streams per chunk (16 threads were enough for 4; see the thread sweep).

## 11. Files changed, memory held

Own delta over the lifecycle-bench cherry-pick (`git diff a57417f7 HEAD`): 12 files, +1734 / -11.
- New: `src/memory/proto_xfer.hpp` (switches, observation), `src/cuda/proto_xfer.cpp` (AVX-512 host OR, helper threads).
- `src/cuda/sirius_dynamic_bloom_filter.cu` (+558): `host_pipeline` (3 schedules), selection in `finish_impl`, checksum / dump.
- `src/memory/device_transfer.hpp`, `src/cuda/device_transfer.cpp` (+211): `host_bounce_scope` (stage once, event-ordered).
- `src/cuda/exact_key_stager.cu` (+80): batched gather. In-list filters, `dynamic_filter_replica_transfer.{hpp,cu}`: pass the scope.
- `test/cpp/operator/bench_dynamic_filter_lifecycle.cpp` (+471): MB4P, `[xfer_proto]`, `[xfer_proto_exact]`.
Nothing in #2569 / #2570 files other than the two call sites above changes shape: the whole delta is [xfer-PR].

Memory held for one publication (measured, MB4P `host_mib` / `scratch_mib`; chunk 8 MiB, W=4):

| schedule | pinned HOST | device scratch (root) | tip |
|---|---|---|---|
| fan-out | W x chunk = 32 MiB | 0 | 8 MiB pinned, 0 |
| gpuor | (S+1) x W x chunk = 64-128 MiB | min(2, chunks) x S x chunk = 16-48 MiB | 8 MiB pinned, 8 MiB |
| hostor | (C+1) x W x chunk = 96-160 MiB | 0 | 8 MiB pinned, 8 MiB |

W=2 halves the pinned window (sweep in section 7). Replica scope: the source extent once (tip: once per target, one
after the other). Batched gather: the slices' bytes, packed (tip: >= 1 block per slice, one at a time).
The pinned blocks come from the engine's fixed-block HOST pool (`allocate_multiple_blocks`), like the tip's window, and
like the tip **without a reservation**; they return to the pool when `finish` returns (after the final wait), or are leaked
with the arrays if a stream cannot be joined.

## 12. Production issues (what a real PR must do beyond this prototype)

1. Reserve the HOST window: take it under the task's host reservation (`allocate_multiple_blocks(bytes, reservation*)`)
   and size W to what is granted (W=2 costs little); keep the tip loop as the floor.
2. Streams and events are created per publication (3 `cudaStreamCreate` + ~50 events): 40 us fixed cost, which makes
   arrays under ~4 MiB slower than the tip (0.12 vs 0.08 ms at 1 MiB). Lease upload streams from `runtime_stream_pool`
   and take the tip loop (or a 1-slot fan-out) below a size threshold.
3. CPU OR: a private pool of 15 helper threads that spin 500 us after a job, one job at a time process-wide. Move it to
   an engine pool, bound it, and place threads / blocks on the staging NUMA node. AVX-512 with non-temporal stores is
   detected at run time; the generic path is 8-byte words.
4. Failure paths after enqueueing are untested (no fault-injection run). The upload streams are joined with the partial
   streams; an unjoinable stream leaks the pipeline like the tip leaks its staging.
5. Tests that pin the workspace (section 8) need new expectations; the scratch-OOM publication change must be decided.
6. `host_bounce_scope` stages whole extents (not chunked): 57 instead of 39 ms/GiB for a large one-shot Bloom replica
   (MB_TRANSFER section 4). Irrelevant on the suite (replicas are < 1 ms), worth chunking if large one-shot Blooms return.
7. Contention: all legs of one chunk are queued up front and uploads are queued in bursts, which is the shape the
   primitives bench found least sensitive to a concurrent copy; not verified in the engine with eager scans.
8. Remove the observation globals (`last_finish`, counters) and the env switches; choose `auto` in code.

## 13. Seam with the per-pair D2D / topology-aware prototype (`proto/xfer-d2d`)

- Three independent decisions: (1) transport per edge (peer copy vs HOST slots), (2) reduce topology (star on the root,
  pair-local pre-merge, host OR), (3) leg schedule (event graph, windows). This prototype owns (3) and the HOST half of
  (1), and adds the host OR to (2); it leaves `find_pair_without_pool_access` and the PEER pipeline untouched.
- Composition: a pair-local P2P pre-merge leaves one holder per pair; the cross-pair stage is then this `host_pipeline`
  with `_inputs` = the pair holders (host OR over 2 inputs instead of 4) and the uploads of the non-holders replaced by
  in-pair peer copies after the holder's upload event (`upload_to` is the only place that changes). The slot / event
  bookkeeping is per leg, so a peer leg simply has no slot.
- Replica copies and the gather choose the route per (source, destination) already; the scope only changes what the HOST
  route does.
- Given section 4 (hostor 45-48 ms/GiB vs `c2` 52 in the primitives bench), the D2D prototype has to beat 45-48 on this
  machine to be worth its extra topology code; its bound is 38.

## 14. Recommendation

PLACEHOLDER_RECO
