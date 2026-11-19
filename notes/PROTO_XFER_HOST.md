# PROTO_XFER_HOST: doing the unavoidable host bounce well (DF finish chain) - prototype + measurements

2026-11-19. Worktree `/opt/dlami/nvme/dfperf/wt-xfer-host`, branch `proto/xfer-host` on top of pr2570 `ae939824`
(+ cherry-pick of the lifecycle microbench `a5b37728`). Local commits only, nothing pushed.
Scripts `$R/runs/xfer_host/bin/`, raw output `$R/runs/xfer_host/`, Quent `$R/telemetry/xfer-host*`.
Every build and GPU process ran under `flock $R/machine.lock` (one queued command per hold, `bin/next.sh`).
Config `/home/ubuntu/sirius/specific_config.yaml` untouched; the prototype is switched by environment variables only.

STATUS: see section 9 for what ran and what did not. Sections 1-3 are design (code facts), 4+ are measurements.

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
