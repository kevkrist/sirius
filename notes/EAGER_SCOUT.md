# EAGER_SCOUT: eager probe-side scheduling after the build PARTITION has published (read-only scout)

2026-11-18. Read-only: no build, no GPU process, no lock taken. Code refs = tip `ae939824` (`$R/repo`), read-only.
New arithmetic comes only from existing data: Quent `telemetry/baseline_tip/q<N>` (tip, 1 warm + 5 hot) and one
existing nsys capture (`runs/profile/nsys/tpch_20261118_180844_nsys_gpu_iter3_q3/.../nsys.sqlite`, opened read-only).
Scripts + raw output: `$R/runs/eager_scout/bin/{sites,allsites,nsys_bins,nsys_win}.py`, `$R/runs/eager_scout/out/`.
Nothing here is an A/B. Every gain figure is a bound or a model and is marked as such.

## 0. Bottom line

1. Today the first probe scan task is released by **the completion of the last build CONCAT task** (its push to the join's
   `build` port), never earlier. The window between "filter visible" and "scan starts" is exactly the build CONCAT pipeline.
2. At the tip that window contains **no publication**: the join's session is already terminal when the build PARTITION
   pipeline finishes (`hash_join_partition_observer::finalize_input`, D_ASSERT `!enabled || terminal`). So releasing the
   probe side at PARTITION finalize loses no filter. That is the property the earlier experiment lacked (one-shot filters
   were built at the CONCAT delivery, inside the window).
3. Upper bound of eager activation on this machine: **676 ms per suite pass at DF sites (4.4%)**, plus 262 ms at joins
   without DF (q14 116, q18 92, q2 27). Bound = sum of min(build-CONCAT window, probe-scan chain), medians of 4 hot iterations.
4. Contention is real and unmeasured in overlap. Existing nsys (q3) shows the lineitem scan is **host-to-GPU copy bound**
   (98% copy duty at 28.2 GB/s per GPU, kernels 15% busy) and the CONCAT `Preparing` needs ~35 ms of ingress copy time per
   GPU out of ~100 ms. Model: 65-100% of the bound on this machine (440-676 ms). Bounce excluded the window itself shrinks
   to ~190 ms (model) and what survives contention is unknown (0-190 ms).
5. Minimal option 1 prototype = a finalize listener on the build PARTITION that schedules the join's probe head. About 30
   lines, no blocking, no hint change, no session change. It belongs to a follow-up PR on top of the stack (S6, #1825).
6. Strategy: OP1 (move the finish off the Publishing Task) is **superseded, not additive**. Once the scan no longer waits
   for the CONCAT there is no slack to hide the finish in, and the finish gates the scan either way. The finish (transfer)
   becomes the only DF cost on the critical path, so transfer improvements pay 1:1.

## 1. How the probe side is scheduled today

Chain of facts (all demand-driven, no explicit pipeline dependency is consulted at run time):

- `task_scheduler::start_query` schedules only `scans.front()` (`src/pipeline/task_scheduler.cpp:235-247`). Pipelines are
  numbered leaf-first and build-first (`reorder_pipelines_topologically`, `src/pipeline/sirius_pipeline_converter.hpp:59-70`),
  so `scans.front()` is the deepest build-side leaf. It is in no join's probe subtree, so it is never a filter target.
- Everything else is created by (a) completion: every finished task schedules its pipeline's consumers
  (`src/pipeline/gpu_pipeline_executor.cpp:575-583`), and a finished pipeline schedules them again
  (`sirius_pipeline::notify_downstream_pipelines`, `src/pipeline/sirius_pipeline.cpp:309-335`), and (b) the hint walk
  `task_creator::get_operator_for_next_task` (`src/creator/task_creator.cpp:322-352`), which follows
  `WAITING_FOR_INPUT_DATA` producers down to the first READY operator.
- BUILD_PROBE hash join hint (`src/op/sirius_physical_hash_join.cpp:929-998`, policy `select_build_probe_action` `:617-672`):
  while any NOT_BUILT slot lacks its build batch the action is `wait_for_build` and the hint names the **build CONCAT**
  (`:986-989`). Only when every slot has its build batch does it return `wait_for_probe`, naming the probe head
  `probe_port->src_pipeline->get_operators()[0]` (`:990-993`). The walk then goes probe CONCAT -> probe PARTITION -> scan.
- The build CONCAT is `concat_all` for BUILD_PROBE (`src/op/partition/consumer_partition_policy.cpp:81-91`): it is READY
  only when the build PARTITION pipeline has finished (`src/op/sirius_physical_concat.cpp:49, 96-107`).
- A pipeline finishes when its source is exhausted and `tasks_created == tasks_completed`
  (`src/pipeline/sirius_pipeline.cpp:402-413`); it then calls `finalize_operator()` on its operators under `_status_mutex`
  (`:410-412`) and notifies consumers outside the lock.
- So the release event of the first probe scan task is: **last build CONCAT task completes -> executor epilogue schedules
  HASH_JOIN -> hint returns `wait_for_probe` -> walk reaches the scan -> the creator drains the scan source** (all scan
  tasks are created at once: q3 44 tasks at 294.8 ms). Not the hash-table build: the table is built lazily by the first
  task that has both a build and a probe batch (`schedule_build`, `:626-631`). Not BUILD_FILTER (right semi/anti only).
- STANDARD / MIXED_JOIN (`:999-1023`, `src/op/join_cross_schedule.cpp:164-183`): `wait_build` until the build producer
  pipeline has finished, then `wait_probe`. Same serial order in practice. On this suite only q4 j9, q20 j49 (RIGHT_SEMI,
  STANDARD) and q2 j21 (MIXED_JOIN) are not BUILD_PROBE (trace log pass, `Join Mode:` lines).
- Right semi/anti: the probe drives partition sizing; BUILD_FILTER publishes, then the build PARTITION's hint follows the
  probe (`src/op/sirius_physical_partition.cpp:426-439`). The scan already starts 0.1 ms after publication (q4).
- Probe-side barriers: probe scan pipeline -> probe PARTITION is PARTIAL unless the probe drives
  (`consumer_partition_policy.cpp:62-63`); probe CONCAT -> join is PARTIAL (`sirius_physical_hash_join.cpp:254-260`).
  The exchange layout is resolved by the build PARTITION before its first claim (`partition_exchange::prepare_impl`,
  `src/op/partition/partition_exchange.cpp:77-128`), so the probe PARTITION can run as soon as the build PARTITION is done.

Hook candidates for "release when the build PARTITION has published":

| hook | where | verdict |
|---|---|---|
| Publishing Task, after `publish_if_final` | `hash_join_partition_observer::after_transform` (`hash_join_partition_observer.cpp:53-58`), called at `sirius_physical_partition.cpp:194` (forward) / `:246` (hash, after the scatter) | earliest (0.05-0.1 ms before the pipeline closes); misses declined / disabled sessions; runs inside a task that can still be OOM-retried. Not worth it |
| PARTITION operator finalize | `sirius_physical_partition::on_finalize_operator` (`:471-474`) -> `observer::finalize_input` (`:60-65`) | **the point**: pipeline finished, retries drained, session terminal by assert. Matches "as soon as the PARTITION operator is finished" |
| finalize listener | `add_finalize_listener` (`src/op/sirius_physical_operator.hpp:803-817`), runs right after `on_finalize_operator`, under the status lock, must not block or throw. Precedent: `src/planner/distributed_plan.cpp:638` | **use this**: registered in `bind_partition_inputs`, touches neither PARTITION nor the observer |

## 2. The earlier eager-scheduling experiment

Found in `/home/ubuntu/sirius` only (not in `$R/repo`): branch `origin/reference/df-optimized-20260917`, commits
`849437af` "Activate BUILD_PROBE probes on 'build partitioned + published'" and `8c07dae5` (knob-off cleanup).
`849437af` says it is a DF-only port of component (b) of `opt/scheduler-concurrent-subtrees` (`a9536b05`); that object is
in neither clone, so the ungated original and component (a) (seeding filter-free build-side scans at `start_query`) could
not be read. No measurement record was found in either clone.

Implementation (22 files, +463):

- Knob `sirius.scheduler.probe_activation` = `on_build_deposited` | `on_partitioned_and_published` (default), new header
  `pipeline/scheduler_config.hpp`, installed on every hash join by the plan generator.
- Channel: `sirius_dynamic_filter_set::mark_producer_terminal` / `all_producers_terminal`; the session marks its target
  channels terminal once, after fan-out, on publish / fail / close.
- Join: `probe_may_start_before_build(build_port)` = policy on, `dynamic_filter_targets_terminal(plan)` (false for a
  disabled plan, so joins without filters kept the deposit rule), build PARTITION pipeline finished. If true, the
  `wait_for_build` case of the hint returns the **probe producer instead of the build CONCAT**.
- Trigger: `sirius_physical_partition::on_finalize_operator` -> `task_creator->schedule(_downstream_consumer_op)`.
- No memory admission, no blocking; "probe batches that arrive first simply wait in the probe repository".

Why it cancelled out (from its own docs diff): on that base a single-partition / broadcast build published **one-shot at
the CONCAT delivery**; only multi-partition builds published at PARTITION. So for every forward-path join (q3 j15, q8 j41,
q5, q9, q12, q16, q21 j33: most of the slack) the gate could not open before the CONCAT had run. Per join it was either
"filter at CONCAT, no early start" or "early start, no filter". The stack removes exactly that conflict.

At the tip nothing of it exists in code (`git grep probe_activation` = 0 hits; `on_finalize_operator` only calls
`finalize_input`). What exists:

- `src/op/dynamic_filter/GLOSSARY.md:192-207`: Build Layout Completion, Early Probe Activation ("after Build Layout
  Completion, once all Relevant Producers are Terminal and its Memory Admission is granted"), Ordinary Probe Activation.
- `docs/super-sirius/dynamic-filters-unified-design.md:103-105` (optional early scan start, purely additive).
- `docs/super-sirius/dynamic-filters-implementation-plan.md:51, 88, 138, 260-273`: S6 = issue #1825, branch
  `stacked/df/06-early-probe` on S5b PR3 commit A, manifest item "Subtrees-b". Acceptance list: both notification orders,
  unrelated and probe-subtree producers, zero-filter terminals, teardown, missed wakeups, memory pressure.
- `docs/super-sirius/pipeline-execution.md:304`: "The scheduler is filter-agnostic" (true at tip; S6 changes it).
- Unused engine primitive: `request_type::lookahead` (`src/creator/config.hpp:33, 99-100`), not selectable through YAML.

## 3. What an eagerly started scan does downstream

Shape at every BUILD_PROBE site (Quent pipelines): `[build PARTITION] -> [build CONCAT] -> [GPU_SCAN -> DYNAMIC_FILTER ..]
-> [probe PARTITION] -> [probe CONCAT] -> [HASH_JOIN ..]`. The scan pipeline does not contain the join.

- Scan output queues in ordinary repositories: probe PARTITION input (PARTIAL), probe CONCAT input (PARTIAL; releases a
  group at `concat_batch_bytes` = 1 GB per partition or when the source is done, `sirius_physical_concat.cpp:40-82`), then
  the join's `default` port. Batches there are ordinary spillable data (downgrade executor). There is no scan-side
  backpressure other than per-task reservations (8.7-17 GB per scan task, 4 concurrent per GPU).
- The join needs both a build and a probe batch to schedule a build task, so probe batches that arrive first just wait.
- **Today the whole probe output is already buffered before the hash table exists** at 12 of the 14 sites with a CONCAT
  window (table below): HASH_JOIN starts
  after the scan ends (q3 612.3 vs 611.1; q8 894.1 vs 892.8), because the probe output stays under the 1 GB CONCAT
  threshold and queued scan tasks outrank PARTITION / CONCAT / JOIN tasks of the same branch (source-first priority,
  `task_creator.cpp:205-272`: q3 PARTITION(13) tasks created at 326 ms run at 526 ms). Exceptions: q9 j29, q21 j33.
  So eager start adds no new buffering regime here. Post-filter probe output per site: 0.5-9.1 GB.

Per site (`runs/eager_scout/out/sites.txt`, hot iteration 3; "moved" = sum of `bytes_to_materialize`; reservations per GPU):

| site | build CONCAT window ms (Preparing med) | moved GB | CONCAT resv GB | probe scan: tasks, ms, max concurrent resv GB | probe out GB | bound ms |
|---|---|---|---|---|---|---|
| q3 j15 | 110.6 (97.6) | 6.62 | 3.9 | 44, 316, 34.2 | 0.55 | 111 |
| q8 j41 | 99.7 (87.0) | 6.02 | 3.7 | 50, 596, 38.6 | 0.96 | 100 |
| q21 j18 | 73.2 (59.1) | 4.38 | 2.6 | 31, 379, 47.3 | 1.82 | 77 |
| q16 j11 | 72.6 (63.4) | 3.71 | 2.5 | 2, 50, 12.8 | 0.96 | 50 (scan shorter) |
| q7 j35 | 55.4 (47.6) | 3.39 | 2.0 | 5, 70.5, 19.7 | 0.12 | 56 |
| q5 j32 | 53.9 (47.2) | 3.28 | 2.0 | 44, 399, 38.6 | 3.79 | 57 |
| q9 j29 | 49.9 (40.5) | 3.01 | 1.9 | 62, 715, 36.2 | 9.13 | 51 |
| q5 j25 | 49.1 (26.7) | 1.80 | 1.8 | 7, 85, 22.3 | 0.55 | 49 |
| q12 j8 | 31.0 (25.7) | 1.68 | 1.0 | 8, 21, 24.1 | 0.63 | 22 (scan shorter) |
| q8 j34 | 28.8 (16.2) | 1.08 | 1.1 | 7, 86, 25.7 | 1.28 | 29 |
| q21 j33 | 27.5 (20.5) | 1.67 | 1.0 | 31, 375, 47.3 | 5.64 | 28 |
| q3 j8 | 11.4 (5.6) | 0.36 | 0.4 | 8, 84, 25.6 | 2.22 | 11 |
| q10 j10 | 8.7 (7.8) | 0.52 | 0.3 | 45, 247, 37.7 | 1.86 | 9 |
| q10 j21, q4 j9 | 0.6 / none | - | - | - | - | 0 |

Per query (`allsites.txt`, medians of hot iterations): q3 123.5, q5 106, q7 58, q8 130, q9 53, q10 10, q12 22, q16 50,
q21 106, q22 4, rest < 3 each. **DF sites 676 ms.** No-DF joins: q14 116, q18 92, q2 27, q9 13, rest small: 262 ms.

At every BUILD_PROBE DF site the pipeline right before the scan is the build CONCAT, right after the build PARTITION
(`sites.txt`; execution is serial, so nothing else runs there). What it uses and what an overlapping scan would share:

- **GPU ingress copy path: the real contention.** nsys, q3 hot window (`out/q3_nsys.txt`):
  - Lineitem scan: every GPU receives host-pinned 1 MiB blocks back to back, 28.2 GB/s, copy duty 98% (235.7 of 240 ms
    on GPU 0), strictly serialized (union = sum), kernels busy 14-16%. The scan is copy bound, not compute bound.
  - CONCAT(10) `Preparing` (97-107 ms per GPU): ingress copy time per GPU ~35 ms (1.06 GB host-to-device at 33-41 GB/s
    plus 0.31-0.57 GB device-to-device at ~50 GB/s), egress ~23 ms. About 2/3 of its wall time is not copy time.
  - Consistent across sites: `Preparing` costs ~57 ms per GB of ingress per GPU (17.5 GB/s effective).
  - If copies share the ingress byte for byte, the scan is delayed by the CONCAT's ingress time: q3 111 - ~37 = ~74 ms
    (about 65%). If the device-to-device and host-to-device paths add up (observed peaks of 37-45 GB/s per GPU against the
    scan's 28), up to 100%. Not measured in overlap.
- Pipeline threads (4 per GPU): the scan creates all tasks at once and occupies 4 of 4. A CONCAT task outranks queued
  scan tasks but does not preempt, so it can wait one scan task (q3 ~30 ms, q8 ~48 ms). Off the critical path where the
  scan is long; no wait where the scan has <= 2 tasks per GPU (q12, q16, q5 j25, q8 j34). The scan loses 1 of 4 threads
  for the CONCAT's duration; with a serialized copy stage that should cost nothing (unverified).
- GPU memory: scan 34-51 GB + CONCAT 1-4 GB + resident build + filters per GPU, against ~92 GB usable. Fits; today no
  reservation wait exists (max 0.05 ms). Tighter machines need the S6 "Memory Admission" rule; not needed to measure here.
- Task-creator threads (4): one extra request per join; no blocking wait (unlike OP1A / OP1B join points).
- Pinned host pool: shared 1 MiB block pool, 600 GB; capacity is not a constraint.
- Hash build: not in the window (lazy, at the first probe batch).

## 4. Transitive and multi-join cases

- Invariant argument. A filter from join A targets scans inside A's probe subtree only. A scan X is reachable only through
  the hint walk of every join on its probe path (X is never `scans.front()`). Each such join releases its probe side only
  after its own session is terminal. By induction every filter aimed at X is visible, or will never exist, when X's first
  task is created. Eager activation changes only when each join releases, not who releases.
- Equivalent serial argument: today nothing else runs during a build CONCAT, so no publication happens in the window the
  eager schedule skips. Everything visible to X today is visible to X under eager.
- Scan targeted by several filters (q2 scan 21: j28 and j25; q21 scan 13: j21 and j18): it waits for the last join on its
  probe path, as today. No extra rule.
- Targets that are build sides of lower joins (q7 j38 -> scans 17, 24; q8 j44 -> scans 15, 23, 30): they are inside the
  publishing join's probe subtree and are demanded only through it. Covered.
- Do **not** reuse the old gate "every producer on each target channel is terminal" as is: a lower join in the same probe
  subtree is a producer on the same channel and can only publish after this join has released its probe side. The gate
  never opens for stacked probes (the S6 plan's "probe-subtree producers"). Gate on the join's own session.
- BUILD_FILTER sites (q4 j9, q20 j49): already eager (probe drives, scan starts 0.1 ms after publication). No change.
- Joins without DF (plan disabled or declined): same hook, no coverage concern. Worth 262 ms upper bound here. The old
  experiment excluded them because the build deposit was their only throttle on unfiltered probe output; on this config
  that output is already fully buffered before the join starts. Keep them behind a separate switch value.
- Not covered by the argument, unchanged from today (opportunistic snapshots): probe sides under STANDARD / MIXED_JOIN,
  nested-loop, delim, union ancestors; lookahead if ever enabled.
- Eager nesting: releasing A's probe side starts the lower joins' build scans while A's CONCAT is still moving. Several
  CONCAT moves and scans can then overlap (q5, q8, q21 chains). Peak build residency is unchanged (all builds are resident
  before the last scan today); the copy path sharing compounds.

## 5. Proposed minimal prototype (option 1)

Invariant: **no task of a scan is created before every filter aimed at it is visible or definitively absent**; the only
knowing exceptions are the ones that exist today (section 4, last bullet).

Design (throwaway, about 30 lines, 2 files + switch):

1. `bind_partition_inputs(join, build, probe, build_concat, probe_drives)` (`consumer_partition_policy.cpp:56-98`):
   `build.add_finalize_listener([&join] { join.activate_probe_after_build_partition(); });` unless `probe_drives`.
2. `sirius_physical_hash_join::activate_probe_after_build_partition() noexcept`:
   - return unless BUILD_PROBE, not MARK, build input non-empty;
   - switch `SIRIUS_EAGER_PROBE`: 0 = tip, 1 = only `dynamic_filter_plan().enabled()` joins, 2 = every BUILD_PROBE join;
   - assert `!plan.enabled() || session.terminal()` (holds: the listener runs after `observer::finalize_input`);
   - probe head = `get_port("default")->src_pipeline->get_operators()[0]`; skip if that pipeline is finished;
   - `get_pipeline()->get_task_creator()->schedule(head)` inside try/catch. One shot (atomic flag).
3. Nothing else: hints, session, observer, CONCAT untouched. The ordinary `wait_for_probe` nomination still fires later
   and finds the scan drained. The build CONCAT is still driven by `notify_downstream_pipelines` and by `wait_for_build`.
   This differs from the old experiment on purpose: the hint keeps naming the build CONCAT, so the build can never be
   left without a nomination.
4. Instruments: Quent is enough. `runs/profile/bin/quent_df.py`: first scan `Created` minus PARTITION end (expect ~0.2
   ms), scan tasks before publication (must stay 0), CONCAT `Queued -> Preparing` delay, scan duration vs tip (contention),
   `Reserving` waits. DEBUG `join filters kept ... N of M probe(s) ran` unchanged (q3 24 of 24 chunks).
5. A/B (not run): q3, q8, q5, q21, q7, q9 (long scans); q12, q16 (scan shorter than CONCAT); q14, q18, q2 at switch 2;
   controls q4, q10, q1, q6. Standard protocol, tip vs prototype, ABBA.

Expected gain (bounds from data, realization is a model):

| | this machine (CONCAT window is mostly HOST-staged bounce) | bounce excluded (model: 20 ms per GB of ingress per GPU) |
|---|---|---|
| DF sites, upper bound | 676 ms (4.4%) | ~190 ms (1.2%) |
| DF sites, with copy-path sharing | 440-676 ms (2.9-4.4%), model from one q3 nsys capture | 0-190 ms: a P2P CONCAT is pure ingress copy; unknown headroom above the scan's 28 GB/s |
| no-DF joins (switch 2) | <= 262 ms (1.7%) | ~1/3 of that, same caveat |
| q3 / q8 / q5 / q21 / q7 / q9 / q16 / q12 | <= 123 / 130 / 106 / 106 / 58 / 53 / 50 / 22 ms | <= 35 / 35 / 25 / 30 / 17 / 16 / 19 / 8 ms |

Relation to the other levers (critical path per site; F = finish in the Publishing Task, C = build CONCAT, S = scan chain):

| design | path | gain vs tip |
|---|---|---|
| tip | F + C + S | - |
| OP1 (prototyped: finish overlaps CONCAT) | max(F, C) + S | min(F, C): measured 24-34 ms per query |
| eager, option 1 | F + max(C, S) | min(C, S) |
| eager, option 2 (deposit first + callback) | max(F + S, C) | min(C, S), plus min(F, C - S) where C > S: q12 ~9 ms, q16 ~8.5 ms |

- OP1 and eager are not additive. With eager in place OP1 buys nothing where S > C (every large site).
- F stays on the path under either option: 600 ms per suite here, ~215 ms P2P-equivalent (OPPORTUNITIES ledger). Levers
  that shrink F (transfer, OP2 dead filters, OP3 sizing, OP5 bits) regain value because F is no longer hidden.

Option 2 would need: deposit-first publication (OP1A continuation or OP1B early task completion, with their lifetime
changes and the 12 unit tests that assert "published before deposit"), a terminal-transition notification from the
session issued outside its locks, and a guard on the ordinary path (`wait_for_probe` must not release before terminal).
It gains at most ~18 ms per suite over option 1 (q12, q16). Option 1 is the better trade, as the user prefers.

Risks:

- Copy-path contention may eat 0-35% here and an unknown share on a P2P machine. This is the number the prototype exists
  to measure.
- Scan tasks grab all pipeline threads before the CONCAT tasks are created (listener runs before the consumer
  notification). Off the critical path in the sites seen; if it shows, schedule the build CONCAT first in the listener.
- Probe pipeline can finish before the build lands (q12, q16). `select_build_probe_action` handles it (`waiting_on_build`
  stays true; broadcast orphan reclaim needs a build batch); never exercised today. Needs a test.
- Empty build: today's handling may avoid scanning the probe; eager would scan it. Gate on a non-empty build.
- Memory: scans start while the build is still being replicated. Fine here (<= ~60 GB of 92 GB per GPU, from reservations); a landable
  version needs the S6 admission rule and a knob.
- Listener runs under the PARTITION pipeline's status lock: `schedule()` only enqueues (same call as the old experiment).
- Delim / MARK / right-family joins excluded in the prototype; a hang there would be a prototype bug, not a design flaw.

Ownership:

| piece | owner |
|---|---|
| "PARTITION finalize implies session terminal" (already true: `hash_join_partition_observer.cpp:60-65`); state it as a contract in docs + one test | #2570 (session / call site) |
| listener, `activate_probe_after_build_partition`, knob, admission, tests, docs (`pipeline-execution.md` "filter-agnostic", `dynamic-filters.md` immediate-probe ordering, `task-creator.md`) | separate follow-up PR on top of the stack = S6 / #1825 |
| eager start for joins without DF | same follow-up, separate commit (general scheduler change) |
| #2569 | nothing |

## 6. Unverified

- No overlap was run. Realization factors are a model from one nsys capture of q3 and Quent timelines of one tip run.
- Whether host-to-device and device-to-device copies add on one GPU's ingress (decides the bounce-excluded figure).
- Whether the scan's serialized copy stage is indifferent to 3 vs 4 pipeline threads.
- Behaviour of the join when the probe pipeline finishes before any build batch arrives (code read only).
- Empty-build path under eager activation (code read only).
- `a9536b05` (ungated original experiment) and any timing record of the earlier experiment: not found.
- The "performance neutral with improved transfer" estimate (summary) is arithmetic on the notes' bounce model
  (tip 15372 - ~385 bounce excess = ~14990 vs base 15225), not a measurement.
