# PROTO_OP1B: OP1 design B (minimal mechanism) - deposit first, publish in the task's tail, join at probe activation

2026-11-18. Worktree `/opt/dlami/nvme/dfperf/wt-op1b`, branch `proto/op1-minimal` (from pr2570 `ae939824`), local commits only.
Scripts `$R/runs/op1b/bin/` (`one_query.sh`, `abba.sh`, `times.py`, `build.sh`), raw output `$R/runs/op1b/<label>/`,
Quent `$R/telemetry/op1b_<label>/q<N>/`. Every build and GPU process under `flock $R/machine.lock`.
Throwaway prototype: measurement first, no docs, no new tests.

## 1. Where the slack is (existing Quent data, tip, `runs/profile/quent/critical_chain.json`; nothing new run)

Execution is one serial chain. At every DF build site with slack the chain is
`build PARTITION (Publishing Task) -> build CONCAT -> first probe scan`:

| q | Publishing Task | build CONCAT task (created at the Publishing Task's end) | first probe scan created |
|---|---|---|---|
| 3 | PARTITION(9) 126.8 -> 184.0 (57.1 ms compute) | CONCAT(10) 184.0 -> 297.4 (**prep 110.3**, compute 3.1) | 297.6 |
| 8 | PARTITION(35) 160.8 -> 196.3 (35.4) | CONCAT(36) 196.3 -> 297.4 (prep 98.2) | 297.5 |
| 21 | PARTITION(11) 52.0 -> 369.5 (317.5) | CONCAT(12) 369.6 -> 450.6 (prep 78.9) | 451.1 |
| 21 | PARTITION(27) 904.4 -> 930.5 (26.1) | CONCAT(28) 930.5 -> 959.2 (prep 27.8) | 959.2 |

- The slack of OPPORTUNITIES OP1 is exactly the build CONCAT task: its `Preparing` phase moves the build batches across GPUs
  (broadcast: every GPU clones the batches of the 3 others; on this machine that is also HOST-staged across the non-P2P pairs).
- The CONCAT is `concat_all`: it is READY only when its producer **pipeline** is finished
  (`sirius_physical_concat.cpp:49,96-107`), and a pipeline finishes when `tasks_created == tasks_completed`
  (`sirius_pipeline.cpp:405-415`), which a task reports from its **destructor** (`gpu_pipeline_task.cpp:444`).
  So "deposit before publish" inside the task is not enough: the CONCAT cannot start while the Publishing Task object lives.
- The probe side is activated only by the hash join's hint (`sirius_physical_hash_join.cpp:986-993, 1017-1020`:
  `wait_for_probe` / `wait_probe` name the probe producer), reached after the build CONCAT pushed.
- No slack at BUILD_FILTER (q4: scan starts 0.1 ms after publication) and at q10 j21 (0.7 ms).

## 2. Candidates, judged from the code

1. **Enqueue-only finish, host wait moved to the consumer** (events). Works for the PEER transport only:
   `chunk_pipeline` already enqueues every merge / replicate leg stream-ordered and waits once
   (`wait_until_replicas_ready`, `sirius_dynamic_bloom_filter.cu:1245-1254`). The HOST transport, the only one this machine
   uses (`find_pair_without_pool_access`, `:1191`), is a host-driven loop: one 8 MiB HOST window and one root scratch chunk are
   reused per chunk and every leg is followed by `cudaStreamSynchronize` (`:1258-1320`). Making it event-driven needs a
   whole-filter HOST buffer (the design explicitly refuses one) or a driver thread: that is a change of the bounce itself
   (excluded) or design A. Scratch from the task allocator, the root exact-filter builds (`stager->gather` waits per GPU) and
   `replicate_to_devices` would also have to outlive the task. Not minimal; not measurable here. **Not implemented.**
2. **What base did.** Base one-shot: `push_data_batch_partitioned("build")` -> `observe_whole_build(delivery, deposit)`:
   `deposit()` first, then build + `replicate_to_devices` + push on the depositing (CONCAT) task thread
   (`wt-base/src/op/dynamic_filter/dynamic_filter_publisher.cpp:915-1000`). Base therefore ran the publication *after* the
   deposit, in the tail of the depositing task, overlapped with whatever the deposit made runnable. Its deposit was the
   join's build port, so what it overlapped with was the probe scan itself: that is the race (8/31, 0/32 chunks probed).
   **Design B = base's shape at the stack's site, plus a join point**: at PARTITION the thing the deposit makes runnable is
   the build CONCAT, not the probe.
3. **Root first, replicas lazily per GPU.** The ingress merge (sources -> root) cannot be lazy, so at most the egress half
   moves; it then lands in the first scan task of each GPU (on the critical path again, only parallel across GPUs), and it
   breaks "visible only after all replicas are ready" on the consumer side (unchanged code today). **Not implemented.**

## 3. What was implemented (12 files, +253 -8; commits `cc1c225b`, then the measurement-only GPU-count override)

Mechanism: **the Publishing Task stays the owner of the publication; only its position relative to the deposit and to the
pipeline's completion moves.**

1. `sirius_physical_partition::execute` no longer calls `after_transform`; it records `{id, space}` on its
   `routed_operator_data` (`set_deferred_publication`).
2. `gpu_pipeline_task::execute`, right after `publish_output` (the sink / deposit), calls the sink's new
   `after_sink(output, stream, complete_task_early)`.
3. `sirius_physical_partition::after_sink`: `claim_publication(id)` (session: pending -> claimed, `++active_operations`,
   so the site finalizer's `finish_input` cannot abandon it) -> `complete_task_early()` ->
   `run_claimed_publication(space, stream)` = the unchanged `state::publish` (finish, replicate, fan-out) on the task's own
   stream, reservation, scratch allocator and thread.
4. `gpu_pipeline_task::report_completion_early()`: `pipeline->mark_task_completed()` now (the destructor then skips it) and
   nominates the consumers like the executor epilogue. The pipeline closes, the build CONCAT is scheduled and runs while the
   task finishes its publication.
5. Join point: `sirius_physical_hash_join::get_next_task_hint` calls `session.await_deferred_publication()` (condition
   variable notified when the session becomes TERMINAL) outside the operator mutex before any hint that names the probe
   producer. BUILD_FILTER (q4, q20 j49) is unchanged: it still publishes before its deposit.
6. `SIRIUS_DF_DEFER_PUBLISH`: `0` = tip behaviour (publish before deposit), `1` = default, `2` = additionally every
   non-publishing contributing task reports completion right after its deposit (OP1b: takes the wait for its own insert
   kernels off the pipeline's completion; the task still syncs its stream before it releases its input).

Everything OPPORTUNITIES listed as lifetime risk is untouched: leases, scratch (H10), task stream, `join_on_failure`,
`active_operation`, recoverable-OOM handling all still run inside the same task. New is only: (a) a task that reports
completion before it returns, (b) `finalize_input` can run while a publication is active (the session already supports
this: `finish_input` "an already running publisher may finish"; the `D_ASSERT(terminal)` in the observer is removed and
`release_retired_storage` is re-run by the publisher and by the hash join's finalizer), (c) a blocking wait in a task-creator
thread at the hint.

Log lines added (INFO): `[op1b] deferred publication ran X ms (started Y ms after claim)` and
`[op1b] join N probe activation: waited for the deferred publication X ms | publication was done X ms`.

## 4. What ran

All under `flock $R/machine.lock`; fresh process per query; `performance_test.py` recipe of BASELINE_SUITE section 0
(host pin + pin compression, config unmodified). Timing protocol: 1 warm + 5 hot, first hot dropped (4 hot per process),
arms back to back in one lock hold per query (`abba_locked.sh`), 2 processes per arm = 8 hot samples per arm, medians.

| label (`$R/runs/op1b/<label>`) | what | arms | queries |
|---|---|---|---|
| `smoke_dbg` | DEBUG log smoke, 3 iterations | proto | q3 |
| `t1_*` | timing (lock per process, first block) | tip, proto, proto, tip | q3 |
| `t2_*` | timing | tip, proto, proto(mode 2), proto(mode 2), proto, tip | q3, q8 |
| `s1_*` | timing, "suite" block | tip, proto, proto, tip | 21 7 9 12 5 16 4 10 2 11 6 (+ see 5.1 for what else finished) |
| `m0_*` | sanity: is the proto binary in mode 0 the tip? | tip, proto(mode 0), proto(mode 0), tip | q3, q8 |
| `g2_*` | **P2P pair**: GPUs 0-1 only (`SIRIUS_PROTO_NUM_GPUS=2`, prototype-internal override, PEER transport) | proto(mode 0), proto, proto, proto(mode 0) | q3, q8, q21, q7 |
| `g2smoke` | DEBUG log, 2 GPUs | proto | q3 |
| `logpass` | TRACE log pass, 2 iterations (iter1 parsed), compared with the tip `runs/baseline/logpass` | proto | see 5.4 |
| `unit_m0.txt`, `unit_m1.txt`, `unit_tip.txt` | `sirius_unittest "[dynamic_filter]"` on the proto build in mode 0 and 1; 2 tests on the tip build | | |

Binaries: tip = `$R/repo` (untouched). The proto binary was rebuilt once (20:27) to add the inert `SIRIUS_PROTO_NUM_GPUS`
override in `sirius_config.cpp`; `t1`, `t2` and the first `s1` queries (21, 7, 9, 12) ran on the first build, the rest on
the second. The DF code is identical in both.

## 5. Results

### 5.1 This machine (4 GPUs, HOST-staged Bloom finish): timing, tip vs proto (mode 1)

| q | tip med ms (n, min..max) | proto med ms (n, min..max) | delta ms | proto/tip | note |
|---|---|---|---|---|---|
| 3 | 660.6 (16, 656..749) | 634.5 (16, 627..700) | **-26.2** | 0.960 | two blocks: -26.3, -25.4 |
| 8 | 936.4 (8, 927..940) | 909.5 (8, 900..912) | **-26.9** | 0.971 | |
| 5 | 690.0 (8, 678..712) | 676.8 (8, 664..699) | **-13.3** | 0.981 | |
| 16 | 312.4 (8, 305..315) | 310.6 (8, 296..312) | -1.8 | 0.994 | inside noise |
| 21 | 1440.2 (8, 1418..1463) | 1415.8 (8, 1403..1435) | **-24.4** | 0.983 | |
| 7 | 748.3 (8, 742..752) | 719.6 (8, 716..725) | **-28.8** | 0.962 | |
| 9 | 1428.5 (8, 1401..1448) | 1394.2 (8, 1370..1421) | **-34.3** | 0.976 | tip processes 1436 / 1415: +-10 ms |
| 12 | 701.9 (8, 699..708) | 693.8 (8, 693..697) | **-8.1** | 0.989 | |
| 4 | 325.6 (8, 323..327) | 325.5 (8, 323..329) | -0.1 | 1.000 | control (BUILD_FILTER, unchanged path) |
| 10 | 854.2 (8, 842..875) | 856.4 (8, 848..866) | +2.2 | 1.003 | control, inside noise (tip 861 / 848) |
| 2 | 149.0 (8, 146..153) | 144.9 (8, 144..147) | -4.2 | 0.972 | small exact builds (3.2 + 1.6 ms) now in slack |
| 11 | 177.3 (8, 170..184) | 175.8 (8, 167..180) | -1.6 | 0.991 | inside noise |
| 6 | 380.3 (8, 379..382) | 379.7 (8, 378..385) | -0.6 | 0.999 | no DF |
| **13 queries** | 8805 | 8637 | **-168** | 0.981 | |

**Full 22-query suite: not completed.** q1 13 14 15 17 18 19 20 22 were queued (ABBA) but never got the machine lock: about
60 waiters from other agents in the last 45 minutes, 0 of my jobs served, so I stopped them. Measured: 13 queries =
8805 ms of the 15372 ms suite, **-168 ms = -1.1% of the suite total, -1.9% of the measured part**. Inference for the 9
unmeasured (not data): q1 13 14 15 have no DF site, so the deferral code never runs; q17 18 19 20 have only 0.5-1.4 ms exact
builds and q22 j28 a 7.1 ms finish with 4.1 ms slack: 0 to about -10 ms in total. Against the OP1 pass bar (q3 >= 25,
q8 >= 25, suite >= 100 ms, 0 scans before visible, q4 / q10 flat): met on this machine.

Sanity (`m0`): proto binary in mode 0 vs tip binary: q3 662.1 vs 659.7 (+2.4), q8 936.1 vs 936.1 (-0.1). The gain is the
deferral, not the build.

OP1b (mode 2, non-publishing contributors also complete early; same block `t2`): q3 634.9 vs 634.4 (mode 1), q8 909.0 vs
909.5. **No measurable gain**: where the publication ends after the CONCAT (q3) an earlier CONCAT start buys nothing, and
where the CONCAT ends last (q8) the insert wait is only 2.8-3.6 ms. The exact-filter replicate is absorbed by mode 1
already (it is part of the deferred `publish`): q2 -4.2 ms is its visible effect.

### 5.2 Per site (Quent; `out/sites_span.md`, `out/op1b_sites_4gpu.md`)

"span" = first task of the build site starts -> first primary consumer scan task created (median of 8 hot).
"deferred ms" and the join point come from the prototype's own log lines.

| q | join | Publishing Task ms, tip -> proto | span ms, tip -> proto | delta | deferred publication ms (alone at tip) | join point |
|---|---|---|---|---|---|---|
| 3 | 15 (fwd, 281 MiB) | 57.2 -> 142.7 | 167.0 -> 142.8 | -24.2 | 137 (57) | waited 17.9 |
| 3 | 8 (fwd, 57 MiB) | 4.2 -> 6.3 | 15.6 -> 13.1 | -2.5 | 5.6 (4.1) | done 6.8 before |
| 8 | 41 (fwd, 174 MiB) | 35.6 -> 93.2 | 135.9 -> 111.3 | -24.6 | 90 (35.5) | done 18.5 before |
| 8 | 34 | 4.1 -> 10.3 | 33.0 -> 30.7 | -2.3 | 9.7 (4.1) | done 20.4 before |
| 7 | 35 (hash, 278 MiB) | 64.0 -> 93.1 | 121.2 -> 93.0 | -28.2 | 81 (52.5) | waited 18.2 |
| 9 | 29 (fwd, 166 MiB) | 31.8 -> 58.6 | 83.1 -> 59.7 | -23.4 | 57 (32) | done 1.0 before |
| 21 | 18 (hash, 1394 MiB) | 317.0 -> 377.2 | 393.1 -> 377.2 | -15.9 | 316 (267) | waited 231 |
| 21 | 33 (fwd, 133 MiB) | 26.1 -> 45.9 | 54.8 -> 45.9 | -8.9 | 44 (26.5) | waited 14.4 |
| 5 | 32 (fwd, 87 MiB) | 16.7 -> 34.7 | 71.7 -> 61.9 | -9.8 | 34 (16.7) | done 27 before |
| 12 | 8 (fwd, 59 MiB) | 11.2 -> 14.7 | 43.4 -> 36.1 | -7.3 | 14.6 (11.5) | done 20.4 before |
| 16 | 11 (fwd, 57 MiB) | 8.5 -> 16.7 | 80.9 -> 78.3 | -2.6 | 16.5 (8.5) | done 62 before |
| 10 | 10 / 21 (hash) | 22.6 -> 25.5 / 17.3 -> 17.5 | 31.6 -> 25.8 / 18.0 -> 17.5 | -5.8 / -0.5 | 23.3 / 15.9 | waited 12.4 / 15.3 |
| 4 | 9 (BUILD_FILTER) | 21.2 -> 21.2 | 21.5 -> 21.5 | 0 | not deferred | - |

- Publishing Task duration (Quent) now includes the tail, so it grows; the part before the pipeline closes is the deposit
  (0.1 ms) plus, in mode 1, the other tasks' insert wait (q3 5.5 ms, q8 3.6 ms: CONCAT created 5.6 / 3.6 ms after the
  site starts, was 57.2 / 35.8 ms).
- **The deferred finish runs 1.2-2.5x longer than alone** (q3 57 -> 137 ms, q8 35.5 -> 90, q5 16.7 -> 34, q21 j18
  267 -> 316) while the build CONCAT is not or little slower (q3 113 -> 104-117 ms, q8 100 -> 109). Realized gain is
  therefore 25-80% of the OPPORTUNITIES bound `min(finish, slack)`: q3 26 of 57, q8 27 of 36, q7 29 of 52, q9 23 of 32,
  q21 25 of 102, q5 12 of 17, q12 7 of 11.5, q16 2.6 of 8.5.
- q3 timeline, hot iteration (ms from query start), tip -> proto: PARTITION(9) tasks 126.8 -> 124.8; CONCAT(10) created
  184.0 -> 130.4, ends 297.4 -> 234-248; publication 184.0 -> 268.7; scans created 297.6 -> 268.7.

### 5.3 Bounce excluded: the same A/B on the P2P pair (GPUs 0-1, PEER transport, measured)

Transport confirmed in the log: `reduced and replicated ... through peer copies`, no `(Bloom through HOST staging)`.
Reference arm = the proto binary in mode 0 (= tip, see `m0`).

| q (2 GPUs) | mode 0 med ms (n, min..max) | mode 1 med ms (n, min..max) | delta ms | ratio |
|---|---|---|---|---|
| 3 | 910.2 (8, 910..911) | 910.5 (8, 909..920) | +0.4 | 1.0004 |
| 8 | 1438.4 (8, 1436..1440) | 1436.1 (8, 1432..1441) | -2.4 | 0.9984 |
| 21 | 2014.5 (8, 2011..2036) | 2019.0 (8, 2008..2039) | +4.5 | 1.0022 |
| 7 | 1191.0 (8, 1185..1196) | 1192.7 (8, 1188..1195) | +1.7 | 1.0014 |
| sum | 5554 | 5558 | +4 | 1.0008 |

**No gain on the P2P pair.** The mechanism does its job there too (q3 j15: publication 13 ms, done 22 ms before the join
point; q21 j18: 42.6 ms, done 27 ms before), but the CONCAT ends at the same time as before:

| q21 j18, 2 GPUs, hot iteration | mode 0 | mode 1 |
|---|---|---|
| last non-publishing task ends | 139.6 | 139.4 |
| publication done | 169.9 (finish 30 ms) | 182.0 (42.6 ms) |
| build CONCAT created -> last end | 169.9 -> 208.3 (38 ms) | 139.4 -> 208.9 (69.5 ms) |
| first probe scan created | 208.3 | 209.0 |

30 + 38 ms serial = 69.5 ms overlapped: the copy work is conserved. The build CONCAT's `Preparing` is itself cross-GPU
copying of the build batches, on the same links (the root GPU's PCIe port) the Bloom merge / replicate needs. q3 j15 on
the pair: CONCAT starts 5.8 ms earlier, takes 6.3 ms longer.

Reading for this machine: the HOST-staged finish is latency-bound (8 MiB chunks, a host wait after every leg), not
bandwidth-bound, so most of its wall time leaves the links idle and the CONCAT's copies fill them. **The 4-GPU gain is
bounce time (the excess of the staged loop) hidden in the CONCAT's shadow.** It is not a gain of P2P-equivalent work.

Limits of this measurement: 2 GPUs, one target (a 4-GPU all-P2P machine has 3 targets and 12 directed links; the root's
port still carries all 6 finish flows plus its own CONCAT flows, so the same conservation is expected, but it is not
measured). 2-GPU plans differ from 4-GPU plans (2 partitions; q3 910 ms instead of 660).

### 5.4 Correctness

- **Results.** `cmp_results.py`, byte comparison of `result.txt`, every run of every arm: q2 3 4 5 6 7 8 9 10 12 16 21
  identical (4 to 14 files each); q11 differs between any two runs **including tip vs tip** (known tie order): the sorted
  rows are identical in all four (936,989 rows). 2-GPU arms: q3 8 21 7 identical between modes and equal to the 4-GPU tip.
  q1 13 14 15 17 18 19 20 22 never ran on the prototype: not compared.
- **No scan before its filter.** Quent (`out/qdf_pro.md`): 288 join-instances (hot iterations of q2 3 4 5 7 8 9 10 11 12
  16 21), **0 consumer scan tasks started before the publication**, same as tip (288, 0). Minimum of (scan created -
  Publishing Task end) is -0.00 ms where the join point waited: the join point releases when the session is Terminal,
  microseconds before the task's own end event. The proto log has exactly one `[op1b] ... probe activation` line per
  deferred publication.
- **Filters and probe counts unchanged.** TRACE pass of the proto vs the tip log pass (`cmp_df.py`): q2 3 4 5 7 8 9 10 12 16 21:
  **11 same, 0 differ**; 32 filters pushed at both; every `[dynamic_filter_key]` line equal (kind, size, pushed, reason);
  compressed-scan probe lines equal in chunk count, probes ran / of, kept rows and total rows (q3 `1/1/true` 24 chunks
  160,514,096 of 1,927,817,321; q8 50 chunks `1/2/false`, as at tip); DYNAMIC_FILTER operator rows in / out equal
  (`out/cmp_df.txt`). 0 fallback, error or reschedule lines in the proto pass. q6 11 and the 9 unmeasured queries: no proto
  log pass (same lock starvation). DEBUG smoke: q3 72 of 72 chunks `1 of 1 probe(s) ran` on 4 GPUs, 48 of 48 on 2 GPUs.
- **Unit tests** (`sirius_unittest "[dynamic_filter]"`, 673 cases):
  - mode 0 on the proto build: 671 pass, 2 fail (`test_dynamic_filter_merge.cpp:2984, 3012`: `REQUIRE(fault::available())`).
    The same 2 fail on the tip binary (`unit_tip.txt`): the fault-injection facility is absent from this build. Not DF
    publication, not the prototype.
  - mode 1: 660 pass, 13 fail = those 2 + 11 that encode the order this prototype changes:
    `test_in_task_publication.cpp:570, 605` (filter visible at the first deposit), `:657` and `:895` (injection keyed on
    "publication before the deposit count reaches N"), 2 cases that inject through the bypassed `after_transform` hook
    (they time out in `wait_until`), and `test_physical_partition.cpp:714, 900, 1018 (x2), 1133, 1232`, which call
    `partition.execute()` directly and expect the publication inside it (the deferral runs from the task's `after_sink`).
    No failure outside that set. These tests would have to be rewritten, not patched.

## 6. Files changed, by owning PR (pr_split.md)

| file | change | owner |
|---|---|---|
| `src/op/dynamic_filter/dynamic_filter_publisher.{hpp,cpp}` | `claim_publication`, `run_claimed_publication`, `await_deferred_publication`, cv notified at TERMINAL | **#2570** (session) |
| `src/op/dynamic_filter/hash_join_partition_observer.{hpp,cpp}` | forwards claim / run; drops `D_ASSERT(terminal)` in `finalize_input` | **#2570** (observer) |
| `src/op/partition/partition_input_observer.hpp` | two virtuals with defaults | **#2570** (call-site interface) |
| `src/op/sirius_physical_partition.{hpp,cpp}` | defers instead of `after_transform`; `after_sink` | **#2570** (call sites) |
| `src/op/sirius_physical_hash_join.cpp` | join point in `get_next_task_hint` | **#2570** (consumer of the session) |
| `src/op/sirius_physical_operator.hpp` | `after_sink` virtual; `routed_operator_data::deferred` | pre-existing engine code; would travel with #2570 |
| `src/pipeline/gpu_pipeline_task.{hpp,cpp}` | `report_completion_early`, call of `after_sink` | pre-existing engine code; would travel with #2570 |
| `src/sirius_config.cpp` | `SIRIUS_PROTO_NUM_GPUS` | measurement only, not part of the change |

**Nothing in #2569** (kernels, accumulators, plan) changes.

## 7. Complexity and risk

Small (about 250 lines, no new thread, no ownership transfer), but it touches two engine contracts:

1. **A task that reports completion before it returns.** `pipeline_finished` becomes true while the Publishing Task still
   runs. Nothing in TPC-H trips on it, but the contract in `sirius_pipeline::mark_task_completed` ("task completed after the
   pipeline was considered done ... query reported complete prematurely") exists for a reason: if the probe side never passes
   the join point (probe pipeline already finished or empty, LIMIT exhausted, a plan where the build site's pipeline closing
   makes the query terminal) the query could retire while the tail still uses the session. Not handled in the prototype.
2. **A blocking wait in a task-creator thread** (`get_next_task_hint`, up to 231 ms on q21). 4 creator threads are shared by
   all queries; a production version must return "not ready" and re-nominate the join from the session's Terminal
   transition (the `post_unlock_actions` dispatch is the natural place).
3. The probe side is activated only through the hint today (demand-driven creator). The retained look-ahead strategy
   (`schedule_lookahead`) would bypass the join point.
4. Visibility is later for any consumer not behind this join's probe activation (none in TPC-H: 0 of 288).
5. `finalize_input` now runs while a publication is active (supported by the session, but the "Terminal by Build Layout
   Completion" statement and its assert no longer hold); storage release moves to the publisher / the join's finalizer.
6. The Publishing Task keeps its reservation, its input pin and an executor thread while the CONCAT runs on the same GPU:
   higher peak on that GPU. Not measured.
7. 11 unit tests and the "Publication before deposit" section of the docs encode the old order.

Compared with a continuation (design A): no scratch / lease / stream hand-over, same overlap. The price is items 1-2.

## 8. Recommendation

**Drop for the stack (do not land in #2569 / #2570).**

- Measured on this machine: q3 -26 ms (4.0%), q7 -29 (3.8%), q8 -27 (2.9%), q9 -34 (2.4%), q5 -13 (1.9%), q21 -24 (1.7%),
  q12 -8 (1.2%); 13 measured queries -168 ms. It closes the base gap (q3 31-35, q8 35, q5 15) but not the OP1 bound (350 ms).
- **All of it is bounce time.** With the bounce excluded (PEER transport, pair 0-1) the same change gives +4 ms on 5554 ms
  (q3 +0.4, q8 -2.4, q21 +4.5, q7 +1.7): the slack is the build CONCAT's cross-GPU copying, which uses the same links as the
  finish, so deferring the finish only delays the CONCAT by the same amount.
- OP1's "170-185 ms P2P-equivalent" in OPPORTUNITIES assumed the slack was free. On the evidence here it is not: expect
  about 0 on an all-P2P machine (measured on 2 GPUs only; 4-GPU all-P2P not measurable here).
- OP1b (insert wait, exact-filter replicate): insert wait not measurable (q3, q8: 0 +- 3 ms); exact replicate is worth
  q2 -4 ms on this machine and is HOST-staged too (assumption carried from PROFILE_SUITE).
- If machines without full P2P are a target after all, this is the cheap way to get about 1% of the suite, provided
  risks 1-2 are engineered properly. That is a decision about the bounce, which this campaign excludes.

## 9. Unverified / not done

- **9 of 22 queries (q1 13 14 15 17 18 19 20 22) never ran on the prototype**: no timing, no result comparison, no log
  pass. The suite figure is 13 measured queries plus an inference.
- 4-GPU all-P2P: not measurable on this machine. The conservation argument for 3 targets is reasoning, not data.
- Why the deferred HOST-staged finish stretches 1.2-2.5x under the CONCAT (copy-engine queueing behind large clones is the
  suspicion): not profiled; it is bounce behaviour.
- Memory peak of the publisher's GPU during the overlap: not measured. No OOM, reservation retry or fallback line in any run.
- Error paths after early completion (fatal publication error, cancellation between claim and run, LIMIT): only what the
  unit tests in mode 1 exercise; no dedicated test.
- 8 hot samples per arm per query (16 for q3); q9 and q10 tip processes differ by 13-21 ms, so their deltas carry +-10 ms.
- Housekeeping: the worktree was first created at `$R/repo/wt-op1b` (relative path with `git -C`) and moved at once with
  `git worktree move` to `$R/wt-op1b`; `$R/repo` has no leftover (`git status` clean). Nothing pushed, no PR. The first
  timing block (`t1`) was killed after q3 to switch to one lock hold per ABBA. `/home/ubuntu/sirius/results/telemetry`
  holds no session of mine (all moved to `$R/telemetry/op1b_*`); nothing written to the root disk.
