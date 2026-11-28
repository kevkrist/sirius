# DECISION: DF perf campaign, final synthesis (stack #2569 -> #2570, TPC-H SF1000, 4 GPUs)

2026-11-28. Synthesis only: nothing built or run for this file. R = `/opt/dlami/nvme/dfperf`. Branches live in `$R/repo`
and on the fork as `dfperf/<branch>`. Every number is quoted from the note named next to it.
Sources: COMBINED (+ its section 10 = verification), PROTO_EAGER + VERIFY_PROTO_EAGER, PROTO_XFER_D2D / _HOST + their
VERIFY notes, DESIGN_TRANSFER_PR, DESIGN / IMPL / REVIEW_SHADOW_RULE, OPPORTUNITIES, pr_split, PROTO_HYBRID_ONESHOT.

## 0. Bottom line

- Three pieces pass the gate, all verified, results identical to the tip: shadow rule, faster Bloom transfer, eager scans.
- Together: **-786 ms (-5.1%) vs tip**, verified (author -830). That is **639 ms (4.2%) below base**; DF on/off goes
  from 1.082x to about 1.14x. The stack alone (tip) is 147 ms (1.0%) slower than base.
- Only the shadow rule is production code (reviewed, fixes done). Transfer = design + 2 verified prototypes.
  Eager = verified prototype + design text; the S6-required plumbing is unbuilt.
- Not yet measured: the combined number with the eager shape recommended for S6 (every join, "f1"); the combined
  run used the narrower eager switch. Estimate about -970 ms (not measured; see table note 3).

## 1. Numbers (ms per suite pass, 22 queries; noise +-3% / +-10 ms per query, suite sums repeat within 20-30 ms)

Baseline (reference of 11-18, `BASELINE_SUITE.md`, `BASE_REFERENCE.md`): **base 15225, tip 15372, DF off 16627**.
Tip re-measured in later sessions: 15290-15320 (same code; session offset ~60 ms). Gains below are always against the
tip of the same session.

| Piece | Bucket | Gain vs tip, verified | Where it comes from | Source |
|---|---|---|---|---|
| S: shadow rule | #2569 + #2570 | **-343** (-2.2%); -363 in the combined session | q21 -306, q8 -35 (two dead Blooms no longer built) | IMPL_SHADOW_RULE 0, COMBINED 5 |
| X: transfer, D2D in pairs + host hop (`pipe`) | transfer PR | **-370..-410** (-2.4..-2.7%); -398 in the combined session | q21 -204, q7 -39, q3 -37, q10 -26, q8 -24 | VERIFY_PROTO_XFER_D2D |
| X': transfer, all-host pipeline, CPU OR (`auto`) | transfer PR (cross-pair leg / no-P2P) | **-394** (-2.6%) | q21 -215, q7 -43, q3 -42, q8 -26 | VERIFY_PROTO_XFER_HOST |
| E1: eager scans, joins with a filter only | S6 PR | -288 (combined session); -324 (mixed sessions, not re-measured) | q21 -58, q3 -48, q5 -47, q8 -42 | COMBINED 5, PROTO_EAGER 6.2 |
| E2: eager scans, every BUILD_PROBE join ("f1", the S6 shape) | S6 PR | **-497..-512** (-3.3%); verifier re-measured 7 queries: -420 vs author -402..-417 | E1 + q9 -80, q18 -83, q14 -53, q22 -15 | VERIFY_PROTO_EAGER 0 |
| Scan throttle K=1 on top of E2 | not in S6 (transfer problem) | -80 on 3 queries (author -100 suite) | q8 -32, q9 -29, q3 -19 | VERIFY_PROTO_EAGER 0 |
| S + X | | -549 | | COMBINED 5 |
| S + E1 | | -640 | | COMBINED 5 |
| X + E1 | | -697 | | COMBINED 5 |
| **S + X + E1 (combined)** | | **-786 (-5.1%)** verified; author -830 (-5.4%) | q21 -375, q8 -74, q3 -72, q7 -58, q9 -28, q2 -26, q10 -25 | COMBINED 10 |

Notes
1. Overlap: S and X both remove the finish of the same two dead Blooms (q21 j18, q8 j41): about 210 ms counted twice
   (q21 170-185, q8 22). E overlaps with nothing (X+E and S+E are additive within noise).
2. Last-in marginals (COMBINED 6, 10.3): S adds 133 on top of X+E; X adds 190 on top of S+E; E1 adds 281 on top of S+X.
   If S lands first: S 363, then X ~186, then E1 ~281.
3. Combined with E2 instead of E1: NOT measured. E2's extra ~190 ms comes from joins without a filter, which S and X
   do not touch, so about -970 ms is expected. Estimate only.
4. Combined vs base on the reference scale: 15372 - 786 = 14586 = base - 639 (-4.2%); DF off / combined = 1.140x.
5. Finish chain per suite pass (debug-log figure): tip 664 ms -> S 312 -> X 265 -> combined 123 (COMBINED 7).
6. All gains are specific to this machine class (P2P only inside pairs 0-1 and 2-3). On a full-P2P machine X is
   designed to equal today's path; S and E do not depend on topology (S's saving shrinks with a cheaper finish).

## 2. Ledger: what goes where

### #2569 (S5B1)

| | |
|---|---|
| Change | Shadow rule, shared half: one definition of the Consumer's static selection + the plan policy "no Membership Filter for a key no Consumer would select" (dormant at #2569; only the Consumer refactor runs there) |
| Branch | `prod/shadow-2569` **8e049ef7** = pr2569 `a2d6bd5a` + b68201ad, 2bee8412, 258f8046, a53c7095 (F004), 8e049ef7 (F002) |
| State | **Reviewed production code**; architect -> developer -> reviewer -> fixes done. Unit: 637 cases + 32 multi-GPU green |
| Files | 20 files, +1555 -82. New `src/op/dynamic_filter/static_filter_selection.{hpp,cpp}`; `src/op/scan/dynamic_filter_merge.cpp` (uses it, -50); `sirius_dynamic_filter.hpp` (proven estimate); `construction_plan.{hpp,cpp}` (`is_shadowed`, `read_published_filters`, cap); stats / key outcome reasons; GLOSSARY; tests |
| Open risks | No sanitizer run. Intermediate commits never built alone (tips only). Touches pre-stack Consumer code. F005 (positional coupling of two functions) and F006 left open, optional. Bloom-behind-Bloom is never shadowed |
| Nothing else | No other measured item belongs here (kernel is at its hardware bound; all kernel variants killed) |

### #2570 (S5B2)

| | |
|---|---|
| Change 1 | Shadow rule, wiring half: session reads the Channels at `begin`, abandons a shadowed construction before `finish`, planner passes the decode cap, docs |
| Branch | `prod/shadow-2570` **cd89bbc8** = pr2570's 7 commits REBASED onto `prod/shadow-2569`, + 06ed4aa9 (skip), b11df5d2 (abandon), 2189bd1b (docs), cd89bbc8 (F003 log fix). `git diff pr2570`: 23 files, +2161 -105 |
| State | **Reviewed production code**. Unit: 661 cases + 32 multi-GPU green. Suite -343 ms, rule fires only at q21 j18 and q8 j41 |
| Open risks | Gain depends on publication order (31 ms margin on q21 today; S6 changes that question). Skewed probe data: the Consumer's fallback filter is gone for good (unmeasured). Second judgement point covered by unit tests + one forced-flip run only. Cap-0 mapping and zone-map-on path have no query-level run |
| Change 2 (not written) | Contract S6 depends on: "build PARTITION finalize => the join's session is Terminal, for every layout and outcome". Today a `D_ASSERT` (`hash_join_partition_observer.cpp:60-65`). Needs a doc line, a test, and a cheap `terminal()` on the channel (`dynamic_filter_publisher.{hpp,cpp}`). Source: PROTO_EAGER 6.8 |
| Keep | The deletion of the one-shot path (user question of 11-27): author's verdict "PARTITION only, no hybrid, no flag" (PROTO_HYBRID_ONESHOT 0). Independent verification was still running on 11-28 01:15 (`$R/runs/vhybrid`, `$R/wt-vhybrid`); no verifier note yet |
| Known cost | #2570 alone ships tip = base + 147 ms (1.0%): the finish sits on the critical path. Fixed by S6 / the transfer PR, not inside #2570 |
| Lead, unowned | Publisher lottery: `finish` runs after the publishing task's own `hash_partition`; which task publishes is a race (q21 +-26 ms, 30-58 ms avoidable delay). VERIFY_PROTO_XFER_D2D 6.5. Not prototyped |

### S6 PR (eager probe scheduling, issue #1825), on top of #2570

| | |
|---|---|
| Change | When the build PARTITION finishes, schedule the build CONCAT and the probe head at once. Release rule: layout stable AND the channel of every scan this activation starts is Terminal ("wait for all filters", gate 2) AND memory admitted; ordinary activation stays as the alternative |
| Branches | `proto/eager-probe` **f05cfd12** (3 files, +67: `src/op/partition/consumer_partition_policy.cpp`, `src/op/sirius_physical_hash_join.{hpp,cpp}`). `proto/eager-opt2` **fdd2caec** (16 files, +560: gates 0/1/2, admission placeholder, throttle, option 2; cuCascade patch at `proto/cucascade-batch-staging.patch` in that branch) |
| State | **Verified prototype** (nothing refuted: gain, ordering, results, no hang on TPC-H + 37 other shapes). Production design exists as text only (PROTO_EAGER 6.8); no architect document, no production code |
| Unbuilt, required by S6 | Real memory admission (prototype's rule always passes); re-evaluation on later notifications (prototype is one shot); notification outside the pipeline status lock with weak targets (prototype violates this); the validation test list. 4 of 8 acceptance criteria exist only as text |
| Open risks | ~190 of the ~500 ms come from joins without a filter: S6 allows that only with demonstrated admission, and that is the one place memory grows (q18 +2.1..3.5 GiB on one GPU). Memory pressure untested. S6 criterion 2 text must change: its literal rule (gate 1) holds 4 stacked-probe joins in every iteration and costs 35 ms; gate 2 never holds any of 56 joins |
| Out | Throttle (at most an internal default-off switch); option 2 (deferred publication); batched staging |

### Transfer PR, on top of #2570, independent of S6 (user decision)

| | |
|---|---|
| Change | One mechanism for the accumulated Bloom finish, replacing BOTH tip transports (PEER pipeline and serial host star, 186 ms/GiB here -> about 52) |
| Design | `notes/DESIGN_TRANSFER_PR.md` (architect, 11-27; no code). About +1100 -450 source, +900 tests |
| Prototypes | `proto/xfer-d2d` **b7d0eb81** (9 files, +1703 -29; mostly `src/cuda/sirius_dynamic_bloom_filter.cu` `island_reduce`); `proto/xfer-host` **699b3dd8** (12 files, +1734 -11; `host_pipeline`, AVX-512 host OR, `proto_xfer.{hpp,cpp}`) |
| State | Both prototypes **verified** (gain, bit-identical filters, real speed-up not shifted waiting). Not mergeable. Production code not started |
| New files (design) | `src/memory/peer_islands.{hpp,cpp}`, `src/memory/host_or.{hpp,cpp}`, `src/op/dynamic_filter/detail/union_plan.{hpp,cpp}`, `detail/union_transfer.hpp`, `src/cuda/union_transfer.cu`; `sirius_dynamic_bloom_filter.cu` `finish_impl` shrinks ~250 lines. Nothing in session, publisher logic, planner, PARTITION |
| Open risks | The composed form is not measured in the engine (step 0 does it). Prototype defect on a failure path (host window released while the other leader's stream may still use it). No test reaches the island error paths. Silent fallback under memory pressure in the prototypes. Coverage: only this 4-GPU box (accepted by the user) |
| Left out | Replica relay, stage-once replica copies, batched exact-key gather: zero TPC-H effect (design Q7, needs the user's confirmation) |
| Follow-up item | Build move queues behind an overlapping eager scan's copies (`cucascade/src/cudf/representation_converter_builtins.cpp`): ~100 ms headroom on top of S6; this is what the throttle recovers |

How the two prototypes become one design (user's lead: D2D inside pairs, host hop between pairs):
- Split the GPUs into islands = groups with mutual peer access. One leader per island.
- Stage A, merge: inside each island, peer-copy into the leader and OR on the GPU. From `proto/xfer-d2d`.
- Stage B, cross: between leaders through pinned host slots, pipelined. From `proto/xfer-host` (its host pipeline,
  fed with the leaders instead of all GPUs). It replaces the three hand-written cross variants of `proto/xfer-d2d`.
- Stage C, fan-out: inside each island, peer-copy the leader's result. From `proto/xfer-d2d`.
- No P2P at all = every GPU is its own island = stage B only = `proto/xfer-host` as is. That is the fallback: same code.
- Full P2P = one island = stages A + C = today's PEER pipeline (deleted and replaced; gate: no regression).
- One open choice (Q1): where stage B does the OR. Architect: on the CPU (flat in the number of islands, no device
  scratch). Lead prototype `pipe`: on the two leaders' GPUs (2 islands only). They tie in the microbench (52-53 vs 52
  ms/GiB). User's answer: measure in the engine first; step 0 (spike branch `proto/xfer-compose`, not started) decides.

User answers already given (OPPORTUNITIES top): Q1 measure first; Q2 scratch refusal publishes through the host, change
the tests; Q4 delete the tip's serial host loop; Q6 this box is enough; Q8 land independently of S6.

## 3. Tried and killed (one line each)

- OP1A / OP1B, finish off the critical path: -167 ms here, zero on a P2P pair; consumes the slack eager scans reclaim.
- Single-GPU build (SGB): -150 ms here, ~-50 P2P-equivalent; publishes 5-73 ms later, against S6.
- Hybrid one-shot for single-partition builds: -159 ms today, forfeits 84 ms under eager, +650 lines; verdict keep PARTITION only (verification running).
- OP4, filter kind for 0.2-2M-key builds: -14 ms suite.
- OP5, 8 bits per key: -265..-275 ms, all finish-chain time, false-positive losses on q10 / q4; superseded by the transfer PR.
- Option 2, deferred publication + callback: no gain over option 1, 12 more files.
- S6-literal release rule (gate 1): never opens for stacked probes, costs 35 ms.
- Scan throttle K=2 / K=3: nothing. K=1 works (-80..-100) but is a transfer fix, not S6.
- cuCascade batched staging: mode 1 nothing, mode 2 unexplained regressions (q9 q18 q10 q2).
- Replica relay, stage-once replica copy, batched exact-key gather: zero TPC-H effect.
- Insert kernel variants (plain add, atomics, block layout, sort by block, read-before-write, second stream): 0% or slower; kernel is at its hardware bound.
- Chunk size 8 -> 32/64 MiB: 0-4%. Single / merged waits: microseconds. Eager leases, many small batches, mutex across begin: <= 12 ms suite.
- Domain cap, declined attempts, publish before the final scatter, bounds: never happen on TPC-H.
- Not prototyped (below the gate): OP3 distinct-aware sizing (q9 only), OP6 (q21 two inserts on one GPU), OP7 fixed-cost bundle.

## 4. Open items that need the user

1. **Apply the shadow rule to the PRs?** `prod/shadow-2569` appends 5 commits to #2569. `prod/shadow-2570` REBASES
   #2570's 7 commits onto it and adds 4: applying it means a force-push of the #2570 branch. Nothing was pushed to sirius-db.
2. **Results never checked against DuckDB CPU for TPC-H.** All checks are "identical to the tip" (q11 differs in tie
   order only). Only the eager verifier's 37 non-TPC-H shapes were compared to DuckDB CPU.
3. **ncu permission denied for agents.** The "insert kernel is at its hardware bound" diagnosis rests on differential
   kernels and nsys only.
4. **Hooks are disabled**: `"disableAllHooks": true` in `/home/ubuntu/sirius/.claude/settings.local.json`. Re-enable when the campaign ends.
5. Transfer design questions still unanswered (DESIGN_TRANSFER_PR 13): Q3 CPU worker budget and owner (proposed 8
   workers, no knob); Q5 charge the pinned window (48-80 MiB) to a host reservation; Q7 confirm leaving out replica
   relay / batched gather; Q9 non-transitive access graphs (greedy cover proposed).
6. S6 scope: (a) include joins without a filter (190 of 500 ms, needs real admission) or filter joins only (-324);
   (b) accept gate 2 in place of the issue's literal rule and update the issue text; (c) throttle: drop, or internal switch.
7. One-shot question (11-27): author says keep #2570 as is; confirm once the verifier note exists.
8. Order of landing: #2570 alone is 1.0% slower than base. Accept as interim, or land S6 / transfer right behind it.
9. No sanitizer run on the shadow-rule branches; per-commit builds not done.

## 5. Next steps for a fresh session (in order)

1. Restore per `/home/ubuntu/dfperf-durable/RESUME.md`; read this file and the USER DECISIONS block of `notes/OPPORTUNITIES.md`.
2. Collect the hybrid one-shot verification (`$R/runs/vhybrid`; rerun if the instance stopped) and close item 4.7.
3. Transfer PR step 0: spike `proto/xfer-compose` (xfer-d2d islands + xfer-host pipeline as stage B). Microbench gates
   rows 1-4 + ABBA on q21 q3 q7 (DESIGN_TRANSFER_PR 11, 12). Pass -> CPU OR; fail -> escalate Q1.
4. Transfer PR production code: developer on steps 1-9 of DESIGN_TRANSFER_PR 12, then reviewer. Record in `notes/IMPL_TRANSFER_PR.md`.
5. S6: architect document from PROTO_EAGER 6.8 + VERIFY_PROTO_EAGER 5 + `notes/ISSUE_1825_S6.md` (admission rule,
   re-evaluation, notification outside locks, test list); then developer -> reviewer. Add the #2570 contract (ledger, change 2).
6. Measure the missing combined arm: S + X + E2 ("f1") vs tip, same session, full suite. Then repeat with production branches.
7. Shadow rule: sanitizer run + per-commit build on both `prod/shadow-*` branches; apply to the PRs once the user says so.
8. Validate TPC-H results against DuckDB CPU (tip and combined), once.
9. Optional leads: publisher lottery (#2570), build-move queueing under eager scans (transfer follow-up, ~100 ms).

## Checkpoint log

- 2026-11-28 01:30 UTC: file written from the notes listed at the top. No GPU work. STATUS.md rewritten, backup run.
