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

PLACEHOLDER_REST
