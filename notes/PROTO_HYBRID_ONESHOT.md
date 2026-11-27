# PROTO_HYBRID_ONESHOT: PARTITION construction for multi-partition builds + ONE-SHOT for whole-build deliveries?

2026-11-27. Worktree `/opt/dlami/nvme/dfperf/wt-hybrid`, branch `proto/hybrid-oneshot` (from pr2570 `ae939824`).
Scripts `$R/runs/hybrid/bin/`, raw `$R/runs/hybrid/<label>/`, Quent `$R/telemetry/hybrid_<label>/q<N>/`.
Every build / GPU process under `flock $R/machine.lock`. Config untouched. Nothing pushed to sirius-db.
STATUS: FINAL (2026-11-27 22:50 UTC).

## 0. Verdict

**(a) PARTITION-only (current #2570). Do not restore one-shot, neither as "hybrid always" (b) nor behind a flag (c).**

| question | answer (TPC-H SF1000, 4 GPUs, same binary, switch) |
|---|---|
| Is one-shot faster than the tip TODAY (no eager)? | Yes: suite 15297 -> 15138 ms, **-159 ms (-1.04%)** (q3 -39, q8 -34, q9 -17, q5 -16, q21 -14). About the size of the 147 ms the stack lost vs base (base not re-run here). |
| Why? | The tip's finish F sits before the build CONCAT on the critical path; one-shot builds on one GPU (no merge ingress, S = 0) and overlaps the other slots' CONCAT tails. Mostly HOST-bounce time on this box (inference by analogy with VERIFY_PROTO_SGB, whose P2P-equivalent gain was ~1/3; not measured here). |
| What does it cost in eagerness? | Filters of the 11 material one-shot sites are ready **10-73 ms later** (sum +400 ms / suite pass); a one-shot filter can never be ready before the build CONCAT ends. |
| With eager scans (S6)? | 9 queries with material sites: tip+eager **-240 ms**, hybrid+eager **-157 ms**: the hybrid forfeits **84 ms** (q5 -29, q2 -23, q3 -16, q8 -13, q16 -13; q9 +6). Hybrid+eager is no better than hybrid alone (-134) except on q21's multi-partition site. |
| With the transfer PR? | It attacks F directly and is additive with eager on the tip path (other agent's interim `runs/combined` m1 table: q3 X -38, E -45, XE -81; q5 XE -62; q8 XE -60). One-shot's `replicate_to_devices` is a different transfer path that the transfer PR would have to cover separately. |
| Coverage | 38 of 46 filter-producing joins (37% of Bloom bytes) are "arrives whole": the hybrid would make one-shot the MAJORITY path and PARTITION the exception. |
| Cost of keeping it | +~650 src lines (490 in the publisher, a second claim/skip/cancel protocol, the join's build-port hook, observer deferral, 3 counters) + ~450 test lines #2570 deleted + a second size/budget policy (already diverged: Bloom budget gate) + a second transfer path. |

So one-shot wins only against a schedule that S6 removes, and only where the Bloom transfer is host-bounced, which the
transfer PR removes. The 159 ms it buys today is bought back twice by S6 (-295 ms suite) and the transfer PR (-370..-410 ms),
both of which need the filter at PARTITION. A flag (c) keeps all of the maintenance cost for a mode that the two follow-up
PRs make strictly worse. If #2570 must not ship a 1% interim regression vs base, the cheap remedy is ordering (land S6 or
the transfer PR right behind it), not a second construction path.
Buckets: nothing for #2569 / #2570 (keep the deletion in #2570); eager release at PARTITION end stays S6; F reduction stays
in the transfer PR. If the user still wants (c): the publisher part belongs to #2569 (it is pr2569 code), the switch +
observer deferral + join hook to #2570, the "eager refuses a non-terminal session" rule to S6.

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

### 1.4 Filter-producing joins by class (debug pass `lp`, hot window; `runs/hybrid/out_join_classes.md` has every row)

46 joins: **38 arrive whole -> one-shot class** (all `4p broadcast BUILD_PROBE`; 1189 MB Bloom), **6 multi-partition** (`4p hash`:
q2 j42, q7 j35 + j38, q10 j10 + j21, q21 j18; 1889 MB Bloom), **2 BUILD_FILTER** (q4 j9, q20 j49; 109 MB).

| q | one-shot class (join: build rows / build MB -> filter) | PARTITION class | BUILD_FILTER |
|---|---|---|---|
| 2 | j7 1 row, j15 5 rows (small IN); j28 2.0M / 298 MB, j25 0.8M / 17 MB (hash IN) | j42 0.64M / 114 MB (none, skipped) | |
| 3 | j8 30M / 114 MB -> Bloom 57 MB; j15 147M / 2104 MB -> Bloom 280 MB | | |
| 4 | | | j9 150M / 1143 MB -> Bloom 109 MB |
| 5 | j10, j18 (small IN); j25 30M / 572 MB -> Bloom 57; j32 45.5M / 1042 MB -> Bloom 87 | | |
| 7 | j9, j21 (small IN); j28 0.8M / 13 MB (hash IN) | j38 12M / 195 MB -> Bloom 23; j35 146M / 4312 MB -> Bloom 278 | |
| 8 | j19, j27 (small IN); j44 1.3M (hash IN); j34 30M / 343 MB -> Bloom 57; j41 91M / 1912 MB -> Bloom 174 | | |
| 9 | j19 10.9M / 41 MB -> Bloom 21; j29 43.5M / 957 MB -> 2 Blooms 166 | | |
| 10 | | j10 57M / 656 MB -> Bloom 109; j21 44.5M / 849 MB -> Bloom 85 | |
| 11 | j7, j28 (small IN); j14, j35 0.4M / 5 MB (hash IN) | | |
| 12 | j8 31M / 535 MB -> Bloom 59 | | |
| 16 | j11 29.7M / 1180 MB -> Bloom 57 | | |
| 17 | j7 0.2M (hash IN) | | |
| 18 | j14, j24 63K (hash IN) | | |
| 19 | j9 0.48M / 13 MB (hash IN) | | |
| 20 | j7 (small IN); j18 2.2M -> Bloom 4; j31 0.35M (2 hash IN); j45 0.13M (none, drained) | | j49 0.13M -> hash IN |
| 21 | j7 (small IN); j21 0.4M (hash IN); j33 69.7M / 532 MB -> Bloom 133 | j18 731M / 5576 MB -> Bloom 1394 | |
| 22 | j28 19M / 73 MB -> Bloom 36 | | |
q1, q6, q13, q14, q15: no filter.

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

## 3. Measurements

### 3.1 Gates
- Unit (`runs/hybrid/unit/summary.txt`): switch off: 638 single-GPU + 31 multi-GPU DF cases pass. Switch 1: 3 cases fail, by
  design (they assert the tip's site/counters: `test_physical_partition.cpp:1014`,
  `test_gpu_execution_dynamic_filter_multi_partition.cpp:306,333`). `:306` also shows the one-shot path has no Bloom budget gate
  (`keys_skipped_bloom_size_gate`): the two paths already differ in policy.
- Queries (debug pass `lp`, T vs H, `runs/hybrid/out_lp_keys.md`): 22/22 windows ok, 0 error/fallback lines; results identical
  on 21, q11 differs in tie order only (it also differs between two tip processes); `[dynamic_filter_key]` lines identical on 21
  (kind, size, pushed), q20 j45 (skipped, targets drained) logs no line in H; decode-pushdown kept/total rows identical.
  Timing runs: every arm's `result.txt` identical to T except q11 (m1 124 + b1 16 processes, all rc=0).

### 3.2 Timing (fresh process per query, 1 warm + 5 hot, first hot dropped, ABBA halves in separate lock holds, 8 hot per arm, medians)

Arms on ONE binary (`wt-hybrid`): T = switches off (tip behaviour), H = `SIRIUS_PROTO_ONESHOT=1`, E = `SIRIUS_EAGER_PROBE=1`,
EH = both, B = `SIRIUS_PROTO_ONESHOT=2`. Full table `runs/hybrid/out_m1_table.md` (`python3 runs/hybrid/bin/table.py m1`).

| q | T ms | H - T | E - T | EH - T | EH - E | note |
|---|---|---|---|---|---|---|
| 3 | 664.3 | **-39.0** | -42.9 | -27.3 | +15.6 | j15 281 MB, j8 |
| 8 | 936.8 | **-34.3** | -40.8 | -27.6 | +13.2 | j41 174 MB, j34 |
| 9 | 1411.0 | **-17.2** | -6.2 | -12.0 | -5.8 | j29: eager queues at the probe here (PROTO_EAGER 5.3) |
| 5 | 693.8 | **-16.2** | -45.4 | -16.1 | **+29.3** | j32, j25 |
| 21 | 1441.0 | -13.5 | -53.0 | -59.0 | -6.0 | j33 one-shot; j18 (1394 MB) multi-partition, eager in both |
| 18 | 1095.4 | -10.2 | | | | T halves differ by 12.5: noise |
| 12 | 701.9 | -6.1 | -10.1 | -7.4 | +2.7 | |
| 10 | 855.2 | -5.7 | | | | no one-shot site: noise |
| 22 | 339.0 | -5.5 | -3.4 | -4.8 | -1.4 | |
| 7 | 745.7 | -4.7 | | | | only IN-lists are one-shot |
| 11 | 175.4 | -4.1 | | | | |
| 2 | 147.6 | -2.7 | -24.6 | -1.8 | **+22.8** | j28 hash IN-list: eager gain lost |
| 16 | 305.4 | +0.1 | -14.1 | -0.9 | +13.2 | |
| 1 4 6 13 14 15 17 19 20 | | -2.9 .. +3.3 | | | | controls / tiny filters |
| **suite (22)** | **15297** | **-159 (-1.04%)** | | | | |
| **9 queries with E/EH** | 6641 | -134 | **-240** | **-157** | **+84** | |

- B (deposit first, the exact pre-stack order; q3 8 5 16, `out_b1_table.md`): -42.9, -36.3, -12.3, -8.9 = -100 ms vs H's -89
  on the same four: no gain worth a filter that races its scan (and it breaks "a scan waits for all its filters").
- E was not run on the 13 other queries; PROTO_EAGER has it (q7 -21, q10 -7, rest flat): those sites are multi-partition,
  so EH = E there and the +84 ms is the suite figure for "hybrid + eager vs PARTITION-only + eager".

### 3.3 Time to filter ready and first scan start per one-shot site (Quent, medians of 8 hot, ms from query start)

`runs/hybrid/out_sites_m1.md` (all four arms). Filter ready: T/E = build PARTITION end; H/EH = build CONCAT end.

| site | Bloom MB | tip F (PROTO_EAGER) | one-shot wall | ready T | ready H | **H - T** | first scan T | H | E | EH | EH - E |
|---|---|---|---|---|---|---|---|---|---|---|---|
| q3 j15 | 281 | 57.2 | 42.9 | 182 | 255 | **+73** | 295 | 255 | 172 | 267 | +95 |
| q8 j41 | 174 | 35.6 | 28.5 | 197 | 266 | **+69** | 298 | 266 | 182 | 271 | +89 |
| q16 j11 | 57 | 8.5 | 8.4 | 64 | 130 | **+66** | 129 | 130 | 64 | 128 | +64 |
| q5 j25 | 57 | 4.2 | 9.9 | 27 | 74 | +48 | 76 | 74 | 27 | 74 | +47 |
| q5 j32 | 87 | 16.7 | 11.4 | 200 | 240 | +40 | 256 | 240 | 180 | 240 | +60 |
| q9 j29 | 166 | 31.9 | 20.6 | 432 | 459 | +27 | 483 | 459 | 433 | 461 | +28 |
| q8 j34 | 57 | 4.2 | 9.9 | 33 | 59 | +26 | 62 | 59 | 32 | 59 | +27 |
| q12 j8 | 59 | 11.2 | 6.4 | 625 | 650 | +25 | 656 | 650 | 625 | 650 | +25 |
| q21 j33 | 133 | 26.1 | 19.1 | 916 | 932 | +17 | 942 | 933 | 879 | 889 | +10 |
| q3 j8 | 57 | 4.2 | 10.4 | 14 | 24 | +10 | 26 | 24 | 14 | 25 | +11 |
| q22 j28 | 36 | 7.1 | 4.0 | 146 | 146 | -1 | 151 | 146 | 146 | 146 | 0 |
| sum | 1164 | 207 | 172 | | | **+400** | | (H - T: -137) | | | (+456) |

Reading:
- One-shot's own wall is only 35 ms/suite below the tip's finish (172 vs 207 ms): single-batch 57 MB sites are SLOWER one-shot
  (9.9-10.4 vs 4.2 ms: at the tip those are S = 0 already and the insert ran inside PARTITION). The big sites save 7-15 ms
  each (no merge ingress). `out_oneshot_wall.md` has every join; all IN-list one-shots are < 5 ms.
- Today's schedule: H's first scan starts 0-40 ms EARLIER than the tip's although its filter is ready later, because the tip
  pays F before the CONCAT and one-shot overlaps the other three slots' CONCAT tails. That is the whole -159 ms.
- Eager schedule: the tip's scan starts at PARTITION end; one-shot sites cannot (EH - E first scan +10..+95 ms, sum +456 ms),
  and 84 ms of wall follows. The realized share is small for the same reason eager realizes ~40% of its bound: the early scan
  contends with the build CONCAT's payload migration.

## 4. Decision detail

| option | today (no eager) | with S6 eager | with transfer PR | code |
|---|---|---|---|---|
| (a) PARTITION-only (#2570) | reference (+147 ms vs base) | -240 on the 9 queries (-295 suite, PROTO_EAGER) | additive with eager on forward sites (COMBINED) | one path |
| (b) hybrid always | -159 ms | -157 on the 9 (**84 ms worse than (a)**) | one-shot replicate not covered; F it avoids shrinks | two paths, one-shot = 38/46 joins |
| (c) one-shot behind a flag | = (a) or (b) | = (a) or (b) | as (b) when on | two paths + a mode nobody runs in CI by default |

Complexity of keeping one-shot (measured on this branch / `git diff pr2569..pr2570`): +534 lines in
`dynamic_filter_publisher.cpp` (a second build + size-policy + fan-out routine and a second claim/skip/reopen/cancel
protocol on the same session), the hash join's build-port override and `_build_arrives_whole`, the observer's deferral, a
finalize split between PARTITION and join, 3 counters, `test_dynamic_filter_publication_claim.cpp` (-367 lines in #2570) and
`test_dynamic_filter_readiness_fault.cpp` (-78), and every policy change twice (the Bloom budget of `9a5069bf` already
applies to one path only). #2570 exists to delete exactly this.

What would change the verdict: a deployment that will never get S6 nor the transfer PR and has no P2P between GPUs; or
data showing most real builds are single-batch small Blooms where neither path matters (then nothing is gained either).

Unverified / not run: E on the 13 other queries in this binary; hybrid combined with xfer-d2d; a P2P machine; nsys of the
one-shot replicate; the 2-GPU case. The q9 result (one-shot beats eager there) is an eager-scheduling property of that site
(scan queues at the probe), not a one-shot merit: see PROTO_EAGER 5.3.

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
- 20:44 unit gate done (`runs/hybrid/unit/summary.txt`): switch off 638/638 single-GPU + 31/31 multi-GPU DF cases pass; switch 1:
  3 cases fail, all assertions about the tip's construction site/counters (`test_physical_partition.cpp:1014` broadcast
  `accumulations_started`, `test_gpu_execution_dynamic_filter_multi_partition.cpp:306,333`: "a single-partition build constructs
  its filters at the PARTITION", and the Bloom budget gate `keys_skipped_bloom_size_gate` which the one-shot path does not have).
- 21:18 debug pass `lp` done (T vs H, 22 queries, `runs/hybrid/out_lp_keys.md`): all windows ok, 0 error lines, results identical
  on 21 (q11 tie order differs), key lines identical on 21 (q20 j45, a skipped/drained filter, logs no key line in H).
  Per-query queue was re-batched (`bin/batch.sh`: several queries per lock hold) because the lock has 3 queues.
- 22:09 timing `m1` 4-arm block done (q3 8 5 9 21 16 12 2 22); 22:27 `b1` (T, B on q3 8 5 16) done; 22:46 `m1` T/H on the
  other 13 queries done. Tables: `runs/hybrid/out_m1_table.md`, `out_b1_table.md`, `out_sites_m1.md` (+ `_full.md`, `.json`),
  `out_oneshot_wall.md`, `out_join_classes.md`, `out_lp_keys.md`.
- 22:50 notes final. Verdict (a). Branch `proto/hybrid-oneshot` committed locally + backed up to the fork by `backup.sh`.
