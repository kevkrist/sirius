# proto/eager-opt2 (worktree wt-eager2) - eager probe OPTION 2 + scan throttle, on top of OP1B

Recovery status: EXACT relative to its base, by transcript replay (agent `proto:eager-probe`, calls 53, 55, 77, 78).
The base is the recovered `proto/op1-minimal` (37850ed0; original e6cec1c3), recovered by another group: its diffstat vs
pr2570 equals the original (12 files, +253 -8), and every replayed edit matched its context exactly once.
Cross-checks that passed: per-commit diffstats (+67; 5 files +17 -8; 4 files +26 and new header), and source line numbers
seen in original logs / greps (`hash_join.cpp:968` "[eager] ... probe activated", `:1000` "[op1b] ... probe activation",
`gpu_pipeline_task.cpp:755`).

- What it is: `proto/op1-minimal` (deferred publication, `SIRIUS_DF_DEFER_PUBLISH`) + cherry-pick of the option 1 patch
  + option 2 (activate the probe side when the deferred publication finishes) + optional scan throttle.
- Commits (original -> recovered, local in `/opt/dlami/nvme/dfperf/wt-eager2`):
  1. 49e651f6 -> 797f03f2  activate the probe side at build PARTITION finalize (cherry-pick of option 1)
  2. 0cd75dcb -> 2ba03d9d  option 2: `partition_input_observer::publication_finished()` hook, called in
     `sirius_physical_partition.cpp` right after `run_claimed_publication`; `hash_join_partition_observer` forwards to
     `join.activate_probe_after_build_partition(nullptr)`; "session not terminal" is now a silent return (the Publishing
     Task calls again); log text says "at build PARTITION finalize" or "after deferred publication".
  3. 2abda103 -> eb221343  scan throttle: new `src/op/eager_probe_throttle.hpp` (process-global), hook in
     `gpu_pipeline_task.cpp` around `prepare_for_processing`, `eager_build_move_finished()` + `_eager_move_state` in the
     join, finalize listener on the build CONCAT in `consumer_partition_policy.cpp`.
- Patches: `proto-eager-opt2.patch` (= `git diff pr2570`, includes OP1B), `proto-eager-opt2.over-op1-minimal.patch`
  (only the 3 commits), `proto-eager-opt2.commits.mbox`. `git apply --check` on pr2570: OK. Not compiled after recovery.
- Switches (env): `SIRIUS_EAGER_PROBE` 0/1/2, `SIRIUS_EAGER_CONCAT_FIRST` (as option 1); `SIRIUS_DF_DEFER_PUBLISH`
  (from OP1B: 0 = publish in PARTITION as at the tip -> option 1 behaviour on this binary; 1 = deferred -> option 2);
  `SIRIUS_EAGER_SCAN_THROTTLE=K` (>0: while an eagerly activated join still waits for its build CONCAT, at most K
  scan-pipeline tasks per GPU run their Preparing phase; 0/unset = off); `SIRIUS_PROTO_NUM_GPUS` (from OP1B).
- Arms in `runs/eager/bin/unit.sh`: o1 (defer 0, eager 1), o2 (defer 1, eager 1), ob (defer 1, eager 0),
  t1 / t2 (defer 0, eager 1, throttle 1 / 2), o2t1 (defer 1, eager 1, throttle 1).
- Build + smoke: `flock $R/machine.lock $R/runs/eager/bin/smoke2.sh` (builds wt-eager2, log `$R/logs/eager2_build.log`,
  smoke2 = option 2, smoke3 = option 1 + throttle 1; flags GO2/GO3). Batches: `queue_b1.sh 3 8 21 16 5 9 7 12 2`
  (arms tip t1 t2 o2 [o1] pro pro ...), `queue_c1.sh 10 11 17 18 19 20 22 4` (tip o2 t2 t2 o2 tip).

## State when the session died (2026-11-19 ~03:22) - WORK IN PROGRESS, NOT FINAL

- Done: build OK (00:48), smoke2 GO2 and smoke3 GO3 (q3, q12, q16: results identical to tip, all activations
  session_terminal=true, 0 partial probes, 0 errors).
- Batch b1 was running: only q3, q8, q21 complete (of 9); c1 queued behind it, nothing complete. Section 6 of
  PROTO_EAGER.md (scan throttle, option 2) was never written - the note still says it is pending. No final return.
- Partial b1 numbers seen in the transcript (hot medians ms, n = 8 per arm; raw data lost):

  | q | tip | pro (opt 1) | o1 | t1 (thr 1) | t2 (thr 2) | o2 (opt 2) |
  |---|---|---|---|---|---|---|
  | 3 | 660.2 | 616.5 (-43.7) | 623.2 (-37.0) | 661.9 (+1.7) | 597.6 (-62.6) | 628.5 (-31.6) |
  | 8 | 935.0 | 893.8 (-41.2) | - | 939.7 (+4.7) | 862.7 (-72.3) | 881.8 (-53.2) |
  | 21 | 1441.8 | 1379.0 (-62.8) | - | 1439.3 (-2.5) | 1393.7 (-48.1) | 1420.0 (-21.8) |
  | sum | 3037 | 2889 (-148) | | 3041 (+4) | 2854 (-183) | 2930 (-107) |

- Partial Quent reading, q3 site P(9)/C(10)/S(11) (b1): CONCAT `Preparing` median tip 99 ms, pro 377, t1 98, t2 319,
  o2 105; scan first start after publication tip 111 ms, pro 0.3, t1 87 (throttle 1 serialises: no gain), t2 0.3, o2 0.1;
  option 2 publishes later (pub @ 259 ms vs 181) with the CONCAT already started 136 ms before publication.
- Not done: b1 for q16 q5 q9 q7 q12 q2, all of c1, section 6 write-up, final verdict on throttle K / option 2, verifier.
