# PROTO_HYBRID_ONESHOT: PARTITION construction for multi-partition builds + ONE-SHOT for whole-build deliveries?

2026-11-27. Worktree `/opt/dlami/nvme/dfperf/wt-hybrid`, branch `proto/hybrid-oneshot` (from pr2570 `ae939824`).
Scripts `$R/runs/hybrid/bin/`, raw `$R/runs/hybrid/<label>/`, Quent `$R/telemetry/hybrid_<label>/q<N>/`.
Every build / GPU process under `flock $R/machine.lock`. Config untouched. Nothing pushed to sirius-db.
STATUS: IN PROGRESS (see checkpoint log at the end).

## 1. Analysis

### 1.1 What "single-partition build" means in the engine

The engine has no "single-partition" class; the class that existed before #2570 is **"the build arrives whole"**
(`_build_arrives_whole`, deleted by #2570, `consumer_partition_policy.cpp` `bind_partition_inputs`):

- `strategy.build_probe && (strategy.num_partitions == 1 || strategy.broadcast)`, or
- not BUILD_PROBE, build side, 1 partition, join publishes DFs, resident bytes < `max_build_probe_resident_bytes`, rows fit `size_type`.

Decided at **run time**, once per join, in the partition-exchange sizing callback (after the build PARTITION's FULL barrier has
measured rows/bytes; `join.get_partition_strategy(in)`), i.e. BEFORE the first build PARTITION task is claimed
(`before_claim`), so the construction point can be chosen per join without any plan-time knowledge. Not known at plan time
(broadcast depends on measured build bytes vs `max_broadcast_join_size` 3 GB; partitions on `hash_partition_bytes` 1 GB).
For these builds PARTITION takes the forward path (`mode != hash || partitions < 2`): batches are forwarded unchanged and
the build CONCAT (`concat_all`) delivers ONE batch per slot to the join's `build` port (broadcast: one whole copy per GPU slot;
the first slot to arrive claims the one-shot construction, the others only deposit).
Plan-time exception: RIGHT_SEMI / RIGHT_ANTI joins construct in a BUILD_FILTER operator (q4 j9, q20 j49) at both PRs' tips;
they must publish before the probe drains and are never one-shot.

So on this config "single-partition" == **forward path == broadcast** for every TPC-H SF1000 filter (only q20 j49 is a true
1-partition build and it is a BUILD_FILTER site). Per-join table: section 1.4 (measured).

### 1.2 What one-shot does / saves (code: pr2569 `observe_whole_build` + `publish_dynamic_filters`, 490 lines)

| | PARTITION site (tip) | one-shot (CONCAT delivery) |
|---|---|---|
| bookkeeping | arrival ledger, certification, inventory, per-task claims, session COLLECTING/PUBLISHING | one claim (`OPEN -> PUBLISHING`) |
| allocation | G x B Bloom arrays leased + zero-filled at `begin`, publication scratch | one array on the source GPU, 3 replicas |
| kernels | insert per batch on the batch's GPU (parallel on 4 GPUs: q3 5.5 ms) | ONE insert over all N keys on one GPU (q3 21 ms); exact kinds straight from the column (no staging/gather) |
| cross-GPU | merge S partials into the root (ingress) + replicate to 3 (egress): (S+3) x B bytes, 2S+2 HOST legs here | replicate only: 3 x B bytes (S = 0) |
| payload | untouched | needs the CONCAT output: for broadcast every GPU has already pulled the whole build (4 x 3/4 of the payload) |
| filter visible | end of the build PARTITION Publishing Task | end of the build CONCAT + one-shot time |

One-shot therefore saves the merge ingress (S x B bytes: 0.93 GB/suite on the forward Bloom sites), the G-way zero-filled
leases, staging of exact keys and all inventory mechanics. It does NOT save the replicate (still 3 targets, HOST-bounced
across GPU pairs here), and it serializes the insert on one GPU.

### 1.3 What it costs in eagerness

Tip filters are visible C ms before the build CONCAT ends (C = PROTO_EAGER 5.1: q3 j15 110, q8 j41 100, q16 j11 72, q5 j32 57,
q9 j29 51, q5 j25 49, q12 j8 30, q8 j34 29, q2 j28 27, q21 j33 25, q3 j8 12 ms). One-shot cannot be ready before CONCAT end by
construction; measured readiness per join in both modes: section 3.3.

## 2. Prototype (`proto/hybrid-oneshot`)

Commits: `7d7cdcdb` (one-shot), `618fc1c8` (cherry-pick of `proto/eager-probe`), 2 small fixes. +671 lines, 9 files.
- `dynamic_filter_publisher.{hpp,cpp}`: pr2569 `observe_whole_build` + `publish_dynamic_filters` + `complete_build_delivery`
  ported verbatim (+534 lines; only adaptions: `publication_path` argument dropped, 2 deleted counters not counted,
  deposit order switchable).
- `sirius_physical_hash_join.{hpp,cpp}` + `consumer_partition_policy.cpp`: `set_build_arrives_whole`, the
  `push_data_batch_partitioned("build")` override (pr2569 hook site).
- `hash_join_partition_observer.{hpp,cpp}` + `build_input_certifier.hpp`: when the switch is on and the build arrives whole,
  `before_claim` abandons the ledger and neither begins nor declines (session stays OPEN); `finalize_input` leaves the
  session to the join (`on_finalize_operator`), as pr2569 did.
- Switch `SIRIUS_PROTO_ONESHOT`: unset/0 = tip; **1** = one-shot, publish THEN deposit (the probe side is released by the last
  build CONCAT task, so every scan starts with its filter: "eager after construction"); **2** = one-shot, deposit first
  (pr2569 order: the filter races the scan).
- With `SIRIUS_EAGER_PROBE=1` the eager listener (build PARTITION finalize) finds a deferred join's session non-terminal and
  does not activate it: one-shot sites keep the default release (CONCAT end = right after construction), multi-partition
  sites are released at PARTITION end. That is exactly the hybrid's "different eagerness per construction point".
- Throwaway: no tests for the restored path, deleted counters not restored, no docs.

## CHECKPOINT LOG (chronological; newest last)

- 19:05 worktree + branch created; submodules need `git -c protocol.file.allow=always submodule update --init --recursive <paths>`.
- 19:15 prototype written (commit 7d7cdcdb): pr2569 `observe_whole_build` + `publish_dynamic_filters` ported verbatim into the
  tip `dynamic_filter_publisher.cpp`; `hash_join::push_data_batch_partitioned` override + `set_build_arrives_whole` restored;
  observer defers (no begin/decline, no finalize) when the switch is on and the build arrives whole.
  Switch `SIRIUS_PROTO_ONESHOT`: 0 tip; 1 one-shot, publish THEN deposit; 2 one-shot, deposit first (pre-stack order).
  `proto/eager-probe` cherry-picked (618fc1c8) so the same binary has `SIRIUS_EAGER_PROBE`. Build queued behind the lock.
- 19:28 build ok (`logs/build_hybrid_2.log`). Smoke q3 (debug log): result identical, key lines identical, decode-pushdown kept
  160514096/1927817321 rows in both modes; filter ready j8 17 -> 26 ms, j15 184 -> 273 ms; one-shot wall 14.5 / 48.5 ms.
- 19:40 queues started: unit gate (`runs/hybrid/unit`), debug pass `lp` (T,H; 22 q), timing `m1` (T H E EH on 15 DF queries,
  T H on 7 controls; 1 warm + 5 hot, first hot dropped, ABBA, one lock hold per half block).
