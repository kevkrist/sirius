# VERIFY_PROTO_OP1A: adversarial verification of OP1 design A (async publication continuation)

2026-11-18/19. Verifier only: the prototype (`/opt/dlami/nvme/dfperf/wt-op1a`, branch `proto/op1-continuation`, HEAD
`acc10949`, library built 21:04) was not modified or rebuilt. Tip = `$R/repo` (`ae939824`). Config
`/home/ubuntu/sirius/specific_config.yaml` untouched. Every GPU process ran under `flock $R/machine.lock`.
Scripts, raw output: `/opt/dlami/nvme/dfperf/runs/vop1a/` (`bin/`, `v1/`, `g2/`, `g2p23/`, `trace/`, `ut/`, `edge/`);
Quent: `/opt/dlami/nvme/dfperf/telemetry/vop1a_<label>/<arm>/q<N>/`.

## 0. Verdict

**Confirmed, both halves.** The gain on this machine is real and of the claimed size; with the bounce excluded it is zero;
no lost filter, no result change. Recommendation "drop for this campaign" stands.

| claim | prototype | verifier | verdict |
|---|---|---|---|
| this machine, q3 / q8 / q7 / q21 | -25.2 / -27.5 / -24.3 / -27.9 ms | -26.7 / -30.1 / -24.1 / -31.3 ms | confirmed |
| this machine, q9 | -25.8 (4 pairs) | -18.8 (pairs -6.8 / -30.9); 6 pairs pooled -23.5; site 24.0 | confirmed, noisy at the wall |
| control q4 | +0.1 | +0.0 | confirmed flat |
| control q10 | +3.0 (+9 / -3), "flat" | -14.5 (-17.4 / -11.6); 4 pairs pooled -5.7; site 6.6 | **corrected**: not flat, about -6 ms (see 1) |
| suite 22 queries | -167 ms paired (-1.09%) | not re-run in full; 7 queries above: -145 ms wall, 144 ms site-attributed (prototype, same 7: -128 ms) | consistent, not independently measured |
| bounce excluded (PEER pair) | q3 -0.1, q8 +0.5, q7 +3.6; q21 not measured | pair 0-1: q3 -0.3, q8 -0.4, q7 +0.7, **q21 -0.1**, q9 -0.2; pair 2-3: q3 +0.4, q21 -3.3 (noise +-5) | confirmed, extended to q21, q9 and the second pair |
| no lost filter / results | identical | identical (sections 2-3) | confirmed |
| unit tests | 12 fail async, contract only | same 12 (+2 that fail at tip too) | confirmed; see caveat in 4 |

## 1. A/B timing, this machine (4 GPUs)

Protocol as the prototype's: fresh process per query, 6 iterations (1 warm + 5 hot, first hot dropped -> 4 hot per
process), four processes back to back in one lock hold per query, order alternated per query (A B B A or B A A B).
A = tip binary, B = prototype (async). Label `v1`, one block. `python3 runs/vop1a/bin/vtable.py runs/vop1a A B v1`.
"paired" = mean over (A1,B1), (A2,B2) of process-median differences.

| q | tip med ms [min-max], n=8 | proto med ms [min-max], n=8 | pooled delta | paired delta (pairs) | paired, first hot kept | results |
|---|---|---|---|---|---|---|
| 3 | 663.0 [653-749] | 637.1 [633-691] | -25.9 (-3.9%) | -26.7 (-26.2 / -27.1) | -27.3 | identical |
| 8 | 938.0 [933-945] | 907.9 [904-910] | -30.0 (-3.2%) | -30.1 (-30.0 / -30.1) | -31.2 | identical |
| 7 | 744.9 [740-759] | 721.3 [717-725] | -23.5 (-3.2%) | -24.1 (-27.5 / -20.7) | -26.4 | identical |
| 21 | 1446.9 [1435-1457] | 1414.0 [1407-1426] | -32.9 (-2.3%) | -31.3 (-32.9 / -29.8) | -29.8 | identical |
| 9 | 1411.5 [1397-1439] | 1390.8 [1376-1424] | -20.7 (-1.5%) | -18.8 (-6.8 / -30.9) | -20.2 | identical |
| 4 (control) | 325.3 [322-328] | 325.1 [323-328] | -0.2 | +0.0 (-0.2 / +0.2) | +0.3 | identical |
| 10 (control) | 857.1 [846-873] | 842.2 [835-848] | -14.9 (-1.7%) | -14.5 (-17.4 / -11.6) | -19.1 | identical |
| sum of 7 | 6387 | 6238 | -148 (-2.3%) | -145 | | |

- Every process rc=0; `construction started` == `constructed filters published`; 0 fallback / error / retry lines;
  `[df_async] continuation ... failed=0` for all.
- Site attribution from Quent on the same block (`vgain.py v1`, prototype's method re-pointed at my data): realized
  q3 j15 24.8, q8 j41 26.1, q7 j35 24.0, q21 j18 22.8 + j33 7.1, q9 j29 22.8, q10 j10 5.9 + j21 0.7; all sites of the
  7 queries: **144 ms realized of 292 ideal (0.49)**. Prototype reported 0.51 over the suite. Agrees with the wall (-145).
- **q10 is not a flat control.** The prototype's own site table already shows 6.7 ms realized there; its wall block
  gave +3.0 (+9 / -3), mine -14.5. Four pairs pooled: -5.7 ms. q10's wall noise is about +-10 ms (tip range 846-873),
  so the wall alone resolves nothing; the site number (6-7 ms) is the best estimate. Minor correction, no effect on
  the conclusion.
- q9: my two pairs disagree in size (-6.8 / -30.9); with the prototype's four pairs: mean -23.5 ms, site 24.0 ms.
- Same-process check (`edge`, one process each, 20 back-to-back q3 after warm-up): tip median 658.8 ms, prototype
  631.2 ms = -27.6 ms.
- Not done: full 22-query suite ABBA (machine queue: 80-95 waiters, 2-3 h per batch). The 7 queries I ran carry
  128 of the prototype's 167 ms; I measured 145 on them. A second block for q10 / q9 / q5 / q6 was queued; see section 7
  for whether it ran.
- Prototype's tables recomputed from its raw CSVs with my script: identical (`vtable.py runs/op1a A B ab1 ab2`, `S B g2`).

## 2. Bounce split

**This machine.** 141.4 of the 143.8 ms realized on my 7 queries sit at Bloom sites, and every Bloom publication on
4 GPUs logs `Bloom through HOST staging`. Non-Bloom sites (exact IN-list / zone-map replicate, fixed costs): F 5.5 ms
taken off the tasks, 2.4 ms realized. So the gain is 98% staged-transfer time moved under the build CONCAT, and the
CONCAT it hides under is itself a HOST-staged cross-GPU move (92-110 of 113 ms `Preparing` at q3 j15).

**Bounce excluded (PEER transport, in the engine).** `CUDA_VISIBLE_DEVICES=<pair> SIRIUS_PROTO_NUM_GPUS=2`, both arms the
prototype binary (S = `SIRIUS_DF_ASYNC_PUBLISH=0`, B = async), order S B B S / B S S B, 8 hot per arm. 0 `HOST staging`
lines in every process. Labels `g2` (GPUs 0,1) and `g2p23` (GPUs 2,3).

| q | pair | sync med ms | async med ms | paired delta (pairs) | site | F off the task | CONCAT grows by | site realized |
|---|---|---|---|---|---|---|---|---|
| 3 | 0-1 | 909.8 | 909.5 | -0.3 (-0.4 / -0.2) | j15, 280 MiB | 6.3 | +5.9 | +0.4 |
| 3 | 2-3 | 909.5 | 910.1 | +0.4 (+0.5 / +0.4) | j15 | 6.2 | +6.4 | -0.1 |
| 8 | 0-1 | 1438.6 | 1438.0 | -0.4 (+0.2 / -1.0) | j41, 174 MiB | 4.0 | +3.9 | +0.1 |
| 7 | 0-1 | 1190.7 | 1190.7 | +0.7 (-3.4 / +4.8) | j35, 278 MiB | 5.6 | +6.1 | -0.6 |
| 21 | 0-1 | 2013.2 | 2017.0 | -0.1 (-5.3 / +5.1) | j18, 1394 MiB | 30.6 | +30.3 | +0.5 |
| 21 | 2-3 | 2019.5 | 2014.7 | -3.3 (-5.5 / -1.1) | j18 | 30.4 | +30.2 | +0.2 |
| 9 | 0-1 | 2204.2 | 2207.2 | -0.2 (+10.0 / -10.3) | j29, 166 MiB | 3.9 | +3.5 | +0.4 |

- Sites, pair 0-1 (q3, q8, q7, q21): 54 ms taken off the Publishing Tasks, **2 ms realized (0.03)**. Pair 2-3: 41 ms, 0 ms.
- The exchange is one for one at every site: the build CONCAT's `Preparing` grows by the finish time that left the task.
  The largest finish (q21 j18, 30.6 ms, with 38 ms of CONCAT slack available) gains 0.5 ms.
- q21 was the prototype's open item: four pairs over both GPU pairs: -5.3 / +5.1 / -5.5 / -1.1, mean -1.7 ms, inside noise.

| | this machine | bounce excluded / P2P-equivalent |
|---|---|---|
| q3 | -26.7 ms (-4.0%) | -0.3 / +0.4 ms |
| q8 | -30.1 (-3.2%) | -0.4 |
| q7 | -24.1 (-3.2%) | +0.7 |
| q21 | -31.3 (-2.2%) | -0.1 / -3.3 (noise +-5) |
| q9 | -18.8 (-1.3%); pooled 6 pairs -23.5 | -0.2 |
| q10 | about -6 (site 6.6) | not measured |
| q4 | 0.0 | - |
| suite | prototype -167 ms (-1.09%), consistent with my 7 queries; not re-run | ~0 (measured 0 on 5 queries, 2 GPU pairs) |

## 3. Would it survive on a machine with full P2P? No.

- Why the PEER result is physics, not a software artifact: at G=2 the CONCAT's cross-GPU move already runs at the link
  rate (q3 j15: ~1.18 GB per direction, estimated as 147 M rows x 16 B / 2, in 20-22 ms `Preparing` = 52-58 GB/s; MB8's raw `cudaMemcpyPeerAsync` load alone:
  52.7 GB/s per direction). A saturated link has nothing to give; the finish's bytes take link time from the CONCAT's
  bytes. MB8's raw-CUDA load (no engine locks) shows the same byte-conserving exchange, so it is not an engine mutex.
- Full P2P, 4 GPUs (not measurable here, argument only): a broadcast or 4-partition build CONCAT makes every GPU ingest
  3/4 of the build and send its own share three times, so every GPU's ingress and egress link is busy for the whole
  move. The finish needs the root's ingress (3 partials) and egress (3 replicas) plus every target's ingress: the same
  links. Budget for q3 j15: root ingress 1.76 GB (CONCAT) + 0.88 GB (partials) = 51 ms overlapped vs 34 + 17-19 ms in
  sequence: 0-2 ms.
- Upper bound for what could survive: the CONCAT's non-move tail (compute ~3 ms per task) per large site, so <= 3 ms per
  site, <= 20-30 ms on the suite, and the G=2 measurement realized none of it (0.03).
- Why this machine differs: both the finish and the CONCAT move are HOST-staged, and staged copies leave the links idle
  about half the time (realized / ideal 0.49). The gain is that idle time. Making the bounce efficient would remove it.
- OPPORTUNITIES' bounce-excluded model for OP1 (170-185 ms) assumed the slack is free capacity. It is not. That figure
  should be struck.

## 4. Correctness

- **Results.** `result.txt` byte-identical between tip and prototype in all 7 queries of `v1` (4 processes each), between
  sync and async in all `g2` / `g2p23` queries, and between tip and prototype in the `trace` pass (q3, q21, q8, q7).
  q11 not re-run (prototype: differs tip vs tip, identical when sorted).
- **Log pass, TRACE** (`trace`, 2 iterations, tip vs prototype, `vlog.py`): stronger than the prototype's debug pass.
  For q3 / q21 / q8 / q7, every operator (25 / 50 / 59 / 49) has the same number of task lines and the same summed input
  and output rows; DYNAMIC_FILTER operators: 363902306, 1185557356, 323157330, 337940410 output rows in both builds.
  `join filters kept` lines: same count, same N-of-M split, same sums. `[dynamic_filter_key]` lines, `construction
  started`, `published (transport): N filter(s) to T target(s)`: identical multisets. The prototype's own `dbg` logs
  re-parsed with my script: identical too.
- **Ordering (Quent), independent method** (`vorder.py`): no join-to-scan mapping assumed. For every continuation, warm
  iterations included, list every task of any operator `Created` inside [handoff, visible).
  - My runs: 240 continuations (`v1`, B1 + B2) + 96 (`g2` B1): **0 scan tasks of any operator**; the only tasks created in
    flight are the 4 (2 at G=2) build CONCAT tasks of that join.
  - Prototype's `ab1` telemetry, all 17 DF queries, B1 + B2: 536 continuations, 0 scan tasks in flight.
  - Smallest distance from `visible` to the next scan task: 0.046 ms.
  - This is stricter than the prototype's check (which looked only at scans it mapped as consumers).
- **Structure of the join point.** `on_input_batch_pushed` runs before `repo->add_data_batch` in all three push
  variants, so a build batch is not visible to the join until the await returns. `select_build_probe_action` keeps
  `wait_for_build` while any NOT_BUILT slot lacks its build batch. For `BUILD_PROBE` the argument holds.
  The only repository inserts that bypass the hook are `exchange_source` and `batch_stream` (distributed paths).
- **Stream ordering of `finish` on a foreign stream** (the one way this could produce false negatives): checked in code.
  Bloom `enqueue_add` records an event on the contribution stream and makes the partial's own stream wait on it
  (`sirius_dynamic_bloom_filter.cu:1109-1112`); the stager's `gather` and the bounds `finish` order the publication
  stream after every partial's stream, the root's included (`exact_key_stager.cu:296-314`, `bounds_accumulator.cu:434-437`).
  No implicit same-stream dependency found. 20 back-to-back q3 in one process: 20 identical results.
- **Unit tests** (`ut.sh`, filter `[dynamic_filter],[publisher],[publication_claim],[physical_partition],
  [in_task_publication],[build_filter],[build_probe],[hash_join],[physical_concat]`):
  - tip binary: 820 of 822 pass; 2 fail on `REQUIRE(fault::available())` (`test_dynamic_filter_merge.cpp:2984, 3012`;
    fault injection not available in this build).
  - prototype, sync mode: 828 of 830 pass, the same 2.
  - prototype, async, run twice: 816 of 830, the same 14 both times = those 2 + the prototype's 12
    (`test_in_task_publication.cpp` 570, 605, 657, 701, 784, 895; `test_physical_partition.cpp` 713, 900, 1018, 1133,
    1232; one `wait_until` 30 s timeout at `dynamic_filter_accumulation_test_utils.hpp:275`). No crash, no hang.
  - Caveat the prototype understates: these 12 are exactly the tests that cover retry, optional scratch OOM, error
    drain during publication and scratch accounting. They fail because their hooks watch the task's allocator and
    stream, which the continuation no longer uses. So under async **the failure paths have no test coverage at all**;
    "fails for contract reasons" is true, "the failure paths work" is not shown.
- **Edge paths run in the engine** (`edge.py`, one process per build, q3's pinned tables):
  - Cancellation: `con.interrupt()` at 24 delays across q3 (50-637 ms). Both builds: every interrupt honoured, no crash,
    no hang; 3 q3 runs afterwards identical in result and time. Prototype log: 103 continuations, 0 failed, 6 cancelled
    before fan-out (no filter pushed).
  - Cancellation latency: an interrupt during the in-flight window returns when the continuation ends (interrupts at
    127-254 ms all return at 267-275 ms); tip returns when the Publishing Task ends (130-184 -> 190 ms). Same class,
    longer window (q3: up to ~145 ms, q21 would be ~320 ms).
  - Empty build (0 rows), one-day build, join under LIMIT, semi join: complete, same results as tip.
  - Incidental, tip binary only, 1 of 2 runs of the LIMIT join: `task_scheduler: pipeline task queue not empty at query
    completion (1 task(s) remaining)` -> CPU fallback (2986 ms). Pre-existing, not the prototype; worth its own issue.
- Not tested by anyone: a fatal continuation error, OOM / spill, STANDARD / MIXED joins, distributed merge.

## 5. Issues a production implementation must solve

1. **No blocking wait in an executor thread.** The join point parks every build CONCAT task in its sink: q21 j18 holds
   4 threads for 222 ms mean (266 max); q3 j15 28 ms; q7 j35 23 ms. The config has 4 pipeline threads. It needs a
   readiness condition in the scheduler, not a condition variable in `on_input_batch_pushed`. Blocked tasks also hold
   their output batch and reservation.
2. **Continuation must run as tracked work**: executor slot, own reservation (scratch is untracked today and is
   charged to unreserved capacity), telemetry (the finish disappears from Quent task time), query-lifecycle accounting.
   Raw `std::thread` per publication is not acceptable.
3. **Failure paths**: rewrite the 12 tests around the join point and add: fatal continuation error (today it surfaces
   one pipeline later, or only in a log line if no build push follows), cancellation in flight (interruptible finish
   or accepted latency), retry of the Publishing Task after the handoff, optional scratch OOM in the continuation.
4. **Ordering beyond BUILD_PROBE**: STANDARD / MIXED joins and lookahead targets were racy before and get a wider loss
   window (the whole continuation instead of the PARTITION task). Distributed inserts bypass the hook.
5. **BUILD_FILTER site** (q4, q20 j49) has no slack and stays synchronous: two publication paths to maintain.
6. **Stream and storage lifetime**: leaked per-GPU stream list; session storage release moves from PARTITION finalize
   to hash-join finalize; the PARTITION estimate still includes scratch the task no longer uses.
7. **Doc contract**: "Publication before deposit" and "Immediate-probe ordering" in `dynamic-filters.md` change.
8. It conflicts with the user decision at the top of OPPORTUNITIES.md (eager probe scans, S6): eager scans consume the
   CONCAT slack this design hides in, and need the filter earlier, not later.

## 6. Files changed (confirmed from `git diff ae939824 acc10949`)

| file | owner |
|---|---|
| `src/op/dynamic_filter/dynamic_filter_publisher.{hpp,cpp}` (+253) | #2570 session |
| `src/op/dynamic_filter/hash_join_partition_observer.cpp` | #2570 observer |
| `src/op/sirius_physical_hash_join.{hpp,cpp}` | #2570 call site |
| `src/sirius_config.cpp` (`SIRIUS_PROTO_NUM_GPUS`), `complete_build_inventory.cpp` (`SIRIUS_PROTO_BLOOM_CHUNK_MIB`), `bench_dynamic_filter_lifecycle.cpp`, cmake | measurement only; `complete_build_inventory.cpp` is a #2569 file but the edit is a bench override |
| #2569 kernels / accumulators / plan | none |

## 7. What ran, what did not

| label | what | holds |
|---|---|---|
| `v1` | 4-GPU ABBA, tip vs prototype, q3 q8 q7 q21 q9 q4 q10, 6 iterations | 7 |
| `g2` | GPUs 0,1, sync vs async (prototype binary), q3 q8 q7 q21 q9 | 5 |
| `g2p23` | GPUs 2,3, sync vs async, q3 q21 | 2 |
| `trace` | TRACE log pass, tip vs prototype, q3 q21 q8 q7, 2 iterations | 4 |
| `ut` | unit tests: tip, prototype sync, prototype async x2 | 1 |
| `edge` | interrupt sweep + edge joins, tip and prototype | 1 |

- One block per query; noise judged from the prototype's independent blocks plus mine.
- Process deviations: (a) I queued up to 17 lock waiters at once (the queue had 80-95 waiters; FIFO-like). (b) My first
  queue was killed and restarted; its orphaned `xargs` kept feeding duplicate jobs; I killed it and three duplicate
  waiters before any ran (checked: one process per label / arm / query). (c) No G=2 run uses the tip binary (it has no
  `num_gpus` override); sync mode of the prototype binary stands in for it (prototype: +1.1 ms vs tip on q3 at 4 GPUs).
- Unverified: full-suite total; full-P2P 4-GPU behaviour (argument in section 3); q5, q12, q16, q18, q2, q22 gains.
