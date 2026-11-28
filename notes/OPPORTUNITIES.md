# >>> USER DECISIONS (2026-11-18, override anything below that conflicts) <<<
- The real goal of constructing DFs at PARTITION: schedule the probe-side scans EAGERLY, as soon as the filter is ready
  (before build CONCAT). Preferred shape: finish inside PARTITION, then schedule the probe side when PARTITION is finished.
  Fallback: callback after PARTITION builds the filters, scans scheduled when it completes.
- A scan targeted by several filters must WAIT FOR ALL ITS FILTERS before starting.
- The host bounce is NO LONGER excluded: memory transfers are now in scope (reap direct D2D where possible; do the
  unavoidable host bounce well). With eager scans the DF finish is squarely on the critical path.
- ALL transfer-mechanics changes (per-pair D2D, topology-aware merge, host-bounce pipeline, replica transfer) go in a
  SEPARATE PR on top of the stack - not in #2569 or #2570. Tag them [xfer-PR]; keep them cleanly separable.
- Eager probe scheduling IS S6 of the DF stack (umbrella issue #1818, S6 = #1825; texts in notes/ISSUE_1818.md and
  notes/ISSUE_1825_S6.md - READ the S6 acceptance criteria; reference PR sirius-db/sirius#1451). It is its own PR (S6)
  on top of S5 - not part of #2569/#2570. Ledger buckets: #2569 (S5B1), #2570 (S5B2), S6 PR (eager probe), transfer PR.
- Consequence: OP1 (finish off the critical path, hidden in the CONCAT slack) and "local replicas after CONCAT" are demoted:
  they consume / sit behind the slack that eager scans reclaim.
- (2026-11-19) Transfer PR direction: user prefers IN PRINCIPLE "D2D inside P2P pairs + host hop between pairs" (proto/xfer-d2d,
  mode pipe) over the all-host CPU-OR scheme, even where the latter ties on this machine. Treat D2D-in-pairs as the lead design;
  the pipelined host bounce is the cross-pair leg / no-P2P fallback.
- (2026-11-19) Instance auto-stops. Checkpoint incrementally to fork kevkrist/sirius (branches dfperf/*, notes branch dfperf/notes,
  issues allowed). A fresh session must be able to resume from dfperf/notes:RESUME.md alone.
- (2026-11-27) Transfer PR design answers (DESIGN_TRANSFER_PR.md questions): Q1 CPU OR between islands vs GPU OR: MEASURE in the
  engine first (step-0 spike decides). Q2 scratch OOM now publishes through host instead of skipping: accepted, CHANGE THE TESTS.
  Q6 coverage: this 4-GPU box is sufficient for now. Q8: transfer PR LANDS INDEPENDENTLY of S6. Q4 (delete tip serial host loop):
  DELETE it (user agrees with architect, 2026-11-27).
- (2026-11-27) NEW QUESTION from user (prototype + decide "when the time is right"): #2570 deletes the one-shot (CONCAT-site)
  construction. Possibly best design = construct at PARTITION for multi-partition builds, ONE-SHOT for single-PARTITION builds,
  eager scan scheduling after construction in both cases (different eagerness per construction point, keeps one-shot's
  efficiency + simplicity). At minimum maybe keep the one-shot path with a flag choosing the construction point for
  single-partition builds. Track: notes/PROTO_HYBRID_ONESHOT.md.
- (2026-11-28) User: APPLY SHADOW RULE TO THE PRs -> DONE. sirius-db stacked/df/05b-construction (#2569) a2d6bd5a -> 08963d79
  (fast-forward, +5 commits); stacked/df/05b-switch (#2570) ae939824 -> 317e0cfc (force-push: 7 PR commits rebased + 4).
  Same trees as reviewed prod/shadow-2569 8e049ef7 / prod/shadow-2570 cd89bbc8, author identity fixed (local apply/2569, apply/2570).
  Old heads kept on fork: dfperf/backup/pr2569-a2d6bd5a, dfperf/backup/pr2570-ae939824. "tip" in older notes = ae939824 (pre-shadow).
  Later PR-bound branches (prod/xfer, S6) must be rebased onto 317e0cfc before they are proposed.
- (2026-11-28) Transfer design Q3/Q5/Q7/Q9: proceed with the architect's recommendations.
- (2026-11-28) Hybrid one-shot question, VERIFIED picture (VERIFY_PROTO_HYBRID_ONESHOT.md corrects PROTO_HYBRID_ONESHOT.md):
  one-shot for whole-arriving builds vs PARTITION-only: -160 ms today (no S6); TIE with S6 when the eager release fires right
  after one-shot publication (author's +84 ms "forfeit" was a prototype artifact); ~100 ms WORSE with S6 + transfer PR
  (cross-experiment estimate, not measured in one binary). Cost of keeping it: ~670 src + ~450 test lines + a second transfer path.
  Recommendation stands (PARTITION-only) but it rests on the transfer PR landing. User decision pending.
- (2026-11-28) User: the TRANSFER PR IS STANDALONE AND LANDS BEFORE #2570 (base origin/main; supersedes "stacked on #2570").
  Step-0 spike result: CPU OR between islands (tie with GPU OR, simpler). Production branch prod/xfer-main; rebased stack proof
  prod/stack-2569-on-xfer, prod/stack-2570-on-xfer. Consequence for the hybrid one-shot question: the state when #2570 lands is
  "transfer on, eager off" -> being measured (HYBRID_FULL.md, arms X vs H+X).
- (2026-11-28) S6 production design written: DESIGN_S6_PR.md (pull/level-triggered release; open questions inside). Not yet shown
  to the user in detail; no S6 production code yet.
- (2026-11-28) User: LANDING ORDER = transfer PR (on main) -> #2569 -> #2570 -> S6.
  Orchestrator note: hybrid-full measurement got a duplicate agent (SendMessage to a workflow agent resumed a second copy);
  the copy was told to stop. Extra arms X / HX (transfer on, eager off) requested in runs/hybridfull/EXTRA_ARMS_REQUEST.md -
  check HYBRID_FULL.md covers them, else run them afterwards. Do not SendMessage running workflow agents again.
- (2026-11-28) User: YES to the three S6 deviations from the issue text in DESIGN_S6_PR.md section 13 (criterion 2: started-scans
  release rule instead of the literal rule; criterion 3: static wakes + hint evaluation instead of a runtime channel
  subscription; criterion 6: real admission rule, untested under memory pressure on this box).

# OPPORTUNITIES: measured, verified opportunity list for the DF stack (#2569 -> #2570), TPC-H SF1000, 4 GPUs

2026-11-18. Synthesis of BASELINE_SUITE, PROFILE_SUITE, BASE_REFERENCE, MB_BLOOM, MB_LIFECYCLE, VERIFY_methodology,
VERIFY_transfer, HYPOTHESES, pr_split (+ construction / probe_publish / concurrency for code facts).
**Nothing was run for this file** (no GPU process, no build, no timing). Every number is quoted from the notes above; the
only new arithmetic is the per-site `min(finish, slack)` table in OP1 (section 3), marked as such. Code line refs are at
tip `ae939824`, read-only.

Reference totals (median of 6 hot, fresh process per query): base `c4fa3638` 15225 ms, tip 15372 ms, tip DF-off 16627 ms.
Per-query noise +-3% or +-10 ms; steady-state spread 1-7 ms for most queries once the first hot iteration (a second
warm-up) is dropped. Gate: >= 1% of a query or >= 0.5% of the suite (77 ms).

## 1. Bottom line

1. DF is worth 1.082x on the suite at tip (confirmed from raw data). Base is 1.092x: **the stack is 147 ms (1.0%) slower
   than base** (q3 +31..35 ms, q8 +35, q5 +15, q16 +8; confirmed by ABBA blocks). Coverage is identical (46 = 46 filters).
2. Cause of that regression (verifier relabel): the tip runs the whole Bloom finish (cross-GPU merge + replicate) inside
   the Publishing Task, before its deposit, and that task is 1:1 query wall time. Base did the replicate off the critical
   path. On this machine the finish is mostly the excluded HOST bounce, but the *placement* is the stack's own mechanic.
3. With the bounce excluded and no scheduling change, every verified construction lever (kernel, bytes, fixed cost) sums
   to 25-60 ms (0.15-0.4% of the suite). None passes the suite gate; only q9 passes a query gate (1-2%).
4. **The only candidate that passes the suite gate is OP1** (deposit first, finish off the Publishing Task's critical
   path, join before probe activation). It is unprototyped: upper bound 330-350 ms here (2.1-2.3%), 170-185 ms
   P2P-equivalent (model, 1.1-1.2%); demonstrated part = the 147 ms base gap.
5. The insert kernel is already at its hardware bound (one dirtied 32-byte sector per key); every kernel variant tested
   is zero or negative. Do not spend prototype time there.

## 2. Cost ledger at tip (what there is to win)

| Item | ms per suite pass | % suite | Status | Source |
|---|---|---|---|---|
| DF build sites on the critical chain (tip minus DF-off) | 645 | 4.2 | measured (Quent, 6 hot) | PROFILE 2.2 |
| of which finish chain (`accum::finish` + `replicate_devices`) | 600 | 3.9 | measured (nsys); all HOST-staged here | PROFILE 3.1, VERIFY_transfer #14 |
| - bounce excess (ingress through host) | ~385 | 2.5 | **excluded**; model split | VERIFY_methodology #7 |
| - P2P-equivalent copy (bytes x 3 targets, ~68 ms/GB) | ~190-215 | 1.2-1.4 | model, not measurable here | VERIFY_methodology #7-8, VERIFY_transfer #12 |
| Insert kernel on the critical path | ~65-69 (q21 44) | 0.45 | measured (nsys per-device) | VERIFY_transfer #4 |
| Fixed costs (begin, contribute host, non-Bloom finish) | ~10-15 | 0.1 | measured | PROFILE 5, MB_LIFECYCLE |
| Probe scan pipelines, tip minus DF-off (net of benefit) | +111 (positives 172) | 0.7 | measured, upper bound per filter | PROFILE 2.2 |
| Probe kernels, Bloom / exact-set, per-device | <= 132 / <= 123 | <= 0.86 / 0.80 | measured upper bound on wall; pre-existing code | VERIFY_transfer sec. 3 |

Two verifier figures for "construction cost without the bounce" coexist and are both kept: 52-60 ms (0.34-0.39%; kernels
+ fixed only, VERIFY_transfer #10) and ~295 ms (1.9%; adds the modelled P2P-equivalent finish, VERIFY_methodology #9).

HOST-staged bounce (excluded, report only): 600 ms (3.9%) on this machine as measured, ~385 ms (2.5%) as pure excess;
31 GB copied per suite pass (q21 15.3 GB = exactly the 10-leg model); q21 296 ms (20.6%), q3 56, q7 56, q8 39, q10 37,
q9 35, q4 21, q5 20. Always on the critical path. Not optimized, not attributed to kernels.

## 3. Opportunities

Owner tags as in HYPOTHESES / pr_split: [69], [70], [split], [pre] = pre-existing code outside the stack.
"This machine" = bounce included (report only where the gain is bounce time). "Excl." = bounce excluded.
Gains are **not additive**: OP1 hides most of the finish share of OP2, OP3, OP5.

### OP1. Deposit first; run finish + replication off the Publishing Task, join before the probe is activated  [split: call site + session 70, async finish API 69]

- **Change.** Today `after_transform` -> `publish_if_final` -> `state::publish` runs synchronously in the last build
  PARTITION / BUILD_FILTER task and ends in host waits, then the task deposits.
  - Call sites: `src/op/sirius_physical_partition.cpp:193-195` (forward), `:246` (hash);
    `src/op/dynamic_filter/hash_join_partition_observer.cpp:53`; `sirius_physical_build_filter.cpp`.
  - Session: `dynamic_filter_publisher.cpp:1172` (`publish_if_final`), `:627-769` (`state::publish`: finish ->
    `replicate_to_devices` loop `:650-660` -> `fan_out` `:711`).
  - Accumulators: `build_filter_construction.cpp:305-400` (`finish`, `root.stream.sync()` at `:398`).
  - New shape: the Publishing Task hands the finish to a continuation that owns the leases and scratch, deposits at once;
    probe activation awaits the publication (join point at the build CONCAT push or the `wait_for_build` hint,
    `sirius_physical_hash_join.cpp:968-998`). The join point is mandatory: consumers never wait, a late filter is lost.
- **Evidence (measured).**
  - Publishing Task holds for the whole finish chain, returns 0.01 ms after it: q21 j18 267 ms, q3 j15 58, q7 j35 52.5,
    q8 j41 36, q9 j29 32 (PROFILE 3.4, nsys).
  - 1:1 on wall: 0.0 ms of other-pipeline task time during the Publishing Task in 18 of 18 cases (VERIFY_methodology M4).
  - Slack between publication and the first consumer scan exists (PROFILE 2.1; table below).
  - Natural experiment: base replicated off the critical path and is faster by q3 31-35, q8 35-36, q16 8, q5 15 ms =
    the tip's Publishing Task time (57 / 35.5 / 8.5 / 16.7 ms) (VERIFY_methodology #4-5, BASE_REFERENCE 3).
- **Per-site bound (my arithmetic from PROFILE 2.1 / 3.4 and MB4 rates; not a measurement).**
  P2P-eq finish = Bloom GB x 68 ms (3 sources) or x 61 ms (0 sources).

| site | finish ms (this machine) | slack ms | min | P2P-eq finish ms | min(P2P-eq, slack) |
|---|---|---|---|---|---|
| q21 j18 (hash, 1394 MiB) | 267 | 76 | 76 | 95 | 76 |
| q3 j15 (fwd, 281 MiB) | 58 | 111 | 57 | 19 | 19 |
| q7 j35 (hash, 278 MiB) | 52.5 | 58 | 52.5 | 19 | 19 |
| q8 j41 (fwd, 174 MiB) | 36 | 102 | 36 | 12 | 12 |
| q9 j29 (fwd, 166 MiB) | 32 | 52 | 32 | 11 | 11 |
| q21 j33 (fwd, 133 MiB) | 26.5 | 29 | 26 | 9 | 9 |
| q5 j32 (fwd, 87 MiB) | 16.7 | 55 | 16.7 | 6 | 6 |
| q12 j8 (fwd, 59 MiB) | 11.5 | 31 | 11.5 | 4 | 4 |
| q16 j11 (fwd, 57 MiB) | 8.5 | 73 | 8.5 | 4 | 4 |
| q10 j10 / j21 (hash) | 20.8 / 16.3 | 8.8 / 0.7 | 8.8 / 0.7 | 7.4 / 5.8 | 7.4 / 0.7 |
| q4 j9 (BUILD_FILTER) | 21.7 | 0.1 | 0.1 | 7.4 | 0.1 |
| q22 j28 | 7.1 | 4.1 | 4.1 | 2.5 | 2.5 |
| 57 MiB single-batch (q3 j8, q5 j25, q8 j34), q7 j38, q9 j19, q20 j18 | 4.1 x3, 3.8, 3.6, 0.9 | 11.5 / 49 / 30, 6, 2.6, 0.9 | 19.6 | ~14 | ~13.5 |
| **sum** | 600 | - | **~350** | ~215 | **~184** |

- **Expected gain.**

| | this machine (bounce time included, report only) | excl. bounce (P2P-equivalent, model) |
|---|---|---|
| Suite, upper bound | 330-350 ms (2.1-2.3%) | 170-185 ms (1.1-1.2%) |
| Suite, demonstrated by base | ~147 ms (1.0%), forward sites only | not measurable here |
| q21 | <= 102 ms (7.0%) | <= 85 (5.8%) |
| q3 | <= 61 (9.2%); base shows 31-35 (5%) | <= 22 (3.4%) |
| q7 | <= 56 (7.5%) | <= 20 (2.7%) |
| q8 | <= 40 (4.3%); base shows 35 (3.8%) | <= 15 (1.6%) |
| q9 | <= 35 (2.5%); base shows only 4-9 | <= 13 (0.9%) |
| q5 | <= 21 (3.0%); base shows 15 | <= 9 (1.4%) |
| q16 / q12 / q10 / q22 | 8.5 (2.7%) / 11.5 (1.6%) / 9.5 (1.1%) / 4 (1.2%) | 3.8 / 4 / 8 / 2.5 |
| q4 | 0 (no slack) | 0 |

  Realization is below the bound where it could be checked: q3 gets ~55% of it at base, q9 ~10-25%. Base was also racy
  (q3 probed 8/31 and 0/32 chunks in two of three hot windows), so base is not the exact shape proposed here.
  Extension OP1b (unmeasured): the same continuation can also absorb the wait for the forward-path insert kernel
  (suite ~22 ms, q3 5.5 ms per task) and the exact-filter replicate (0.9-3.2 ms per hash IN-list join, ~18 ms suite).
- **Risk: high.**
  - Coverage: without a join point filters arrive late and are lost (consumers never wait). Must keep "0 scans before
    visible" (276 of 276 today).
  - Lifetime: leases, publication scratch (H10 reservation), task stream ownership, `join_on_failure`, the
    `active_operation` guard and `finalize_input`'s terminal assert all assume the task owns the publication.
  - Contention with the build CONCAT's own cross-GPU moves is unmeasured (CONCAT `Preparing` 60-110 ms with
    `host_staging` ranges); it shares links, and on this machine the pinned host path.
  - On this machine the gain is mostly bounce time moved into slack: report it as placement, never as a kernel win.
- **A/B.**
  - Queries: q3, q8, q5, q16 (forward, base-parity targets); q21, q7 (hash path); q9, q12; controls q4, q10 (no slack,
    must not regress), q2, q11 (small builds), one no-DF query.
  - Timing: `one_query.sh`, 1 warm + 5 hot, ABBA, drop the first hot iteration, under the lock.
  - Attribution: Quent Publishing Task duration (expect pre-finish only: q21 56 ms, q3 5 ms) and first-scan-start minus
    publication via `runs/profile/bin/quent_df.py`; debug log `join filters kept ... N of M probe(s) ran` unchanged
    (q3 24 of 24 chunks).
  - Bounce-excluded figure: session-level microbench (extend MB4/MB5 in `wt-mb-life`) on GPUs 0,1 (PEER):
    `publish_if_final` return latency vs visible latency, with and without a concurrent cross-GPU copy load.

### OP2. Do not build filters that have no consumer effect (q8 j41, q9 j29 second key)  [69 plan policy; consumer gate pre]

- **Change.** Step 0 is a diagnosis, not an optimization: log the consumer gate state per filter (it is logged at no
  level) and find why q8 j41's 174 MiB Bloom is never probed (scan runs 1 of 2 probes, DYNAMIC_FILTER 39.96M -> 39.96M,
  same at base). If it is a consumer defect the filter may become useful instead. Otherwise skip at plan time in
  `construction_plan.cpp:119-196` using `estimated_selectivity_of` (`dynamic_filter_publisher.cpp:200, 683`); on
  multi-key joins skip the weaker key.
- **Evidence.** q8 j41: 35.5 ms Publishing Task, 3.4 ms insert, never probed (BASE_REFERENCE 5.2, VERIFY_transfer #13).
  q9 j29: one of two 83 MiB keys keeps 98.9% of probed rows; its probe kernels cost ~8 ms before the gate cuts it.
- **Expected gain.** Excl.: 12-22 ms suite (0.08-0.14%); q8 ~0.4%, q9 ~1%. This machine: +35 ms q8 (3.8%), +16 ms q9
  (1.1%). After OP1 the build share is hidden in slack and only ~3.4 ms (q8) and ~8 ms (q9) remain.
- **Risk: medium.** A wrong estimate drops a useful filter.
- **A/B.** q8, q9; nsys count of `membership_probe_functor<bloom_lookup>` kernels per filter; controls q3, q10.

### OP3. Distinct-aware Bloom sizing (q9 j29 only on TPC-H)  [split: sizing policy 69, `bloom_for` line 70]

- **Change.** Size on an estimate of distinct keys instead of valid rows (`bloom_sizing.hpp:33-45`,
  `construction_plan.cpp:70-75`). Needs a distinct estimate at plan time; the existing domain cap never binds.
- **Evidence.** DuckDB CPU counts: q9 j29 43.48M rows, 10.87M / 9.89M distinct (4.0x / 4.4x): 166 MiB -> ~40 MiB.
  Every other Bloom build is 1.00-1.07x. q7 j35 and q10 j21 build sides were not identified.
- **Expected gain.** Excl.: <= 13 ms (0.08%), mostly probe; q9 0.9%. This machine: +24 ms (q9 2.6% in total).
- **Risk: low to medium** (an underestimate raises the false-positive rate). One site only: low priority.
- **A/B.** q9; `df_bloom_bench` at 83 vs 21 MiB for the probe side.

### OP4. Filter kind for 0.2-2M-key builds: in-cache Bloom vs exact hash IN-list  [69 plan policy; probe kernels pre]

- **Change.** `choose_membership` (`construction_plan.cpp:80-99`) / `dynamic_filter_inlist_max_l2_fraction`. First
  explain why exact-set probes run at 34-36 G rows/s in q8, q17, q21, q11 but 64-76 in q7, q18, q19, q20; the fix may
  belong in the set probe, not in the kind choice.
- **Evidence.** nsys kernel records only: exact-set probes 23.6 B rows, 475 GPU-ms, <= 123 ms per-device; in-cache Bloom
  probes run at 80-120 G rows/s. **No A/B, no microbench.**
- **Expected gain (upper bound, same on both columns).** ~60 ms per-device (0.4%); q17 ~14 ms (2.9%), q8 ~15 ms (1.6%).
  Per-device sums bound wall time from above; whether the GPU bounds those scan pipelines was not checked.
- **Risk: medium.** A Bloom adds 0.13% false positives where keep ratios are 0.001 (q17) and 0.0001 (q18): survivors
  double or worse.
- **A/B.** The knob is YAML-only and the config must not be edited: needs a prototype override. Queries q17, q8, q11,
  q21; controls q18, q19. Microbench: add `set_lookup` probes and clustered probe keys to `df_bloom_bench`.

### OP5. Bits per key (16 -> 8, or only above a size threshold)  [constant pre; policy 69 if adopted]

- **Evidence.** Insert: halving cuts kernel time 30-59% in the 57-280 MiB knee, 0-1% above; ~8.5 ms per suite.
  Probe: <= ~15 ms gross. False positives x25 (0.132% -> 3.32%, measured), ~0.75 B extra rows per suite pass;
  downstream cost unmeasured. Only 8 / 16 / 32 compile (`bloom_sizing.hpp:37`).
- **Expected gain.** Excl.: insert ~8.5 ms (0.06%) + probe <= 15 ms (sign unknown) + half the P2P-equivalent finish
  (model 32-105 ms, 0.2-0.7%). This machine: +300 ms (1.95%), almost all bounce: report only. After OP1 the finish
  share shrinks to what exceeds the slack (q21 j18, q10, q4: roughly <= 35 ms P2P-equivalent, my estimate).
- **Risk: medium to high.** Net sign unknown until a full suite run at 8 bits.
- **A/B.** Only after OP1. Full suite at 8 vs 16 with a prototype override; watch q21, q3, q7, q10, q4 (Bloom on lineitem).

### OP6. q21 j18: two inserts back to back on one GPU  [pre: batch placement, outside the stack]

- **Evidence.** 5 batches on 4 GPUs; one GPU runs 24.8 + 24.2 ms of insert in every hot iteration. Net 44 ms (3.1% of
  q21); ~24 ms is the fifth batch landing on a busy GPU.
- **Expected gain.** <= 24 ms (q21 1.7%, suite 0.16%), same on both columns. No kernel lever exists for the rest.
- **Risk: low for DF; it is a scheduler change.** Hand off; do not prototype inside the stack.

### OP7. Fixed-cost bundle  [69 accumulators, 70 session]

- Single wait for the finish chain (6-20 us per join), lazy lease / no zero-fill on pure targets (~10 us per key per
  GPU; 0.7 ms per GB async), shared stream + event (<= 1.3 us per key per batch), coalesced exact slices (4.9 us per
  remote slice), single-GPU-inventory fast path (0.2-0.5 ms per small join at G=4).
- **Expected gain.** ~10, at most 25 ms per suite (0.07-0.16%), same on both columns; q2 ~2 ms (1.5%). Not resolvable in
  suite timings; provable only by nsys. Do only what also simplifies the code.
- Untested side: eager leases hold G x B unspillable (q21 zero-fills 6.1 GB at begin). A memory item, not a time item.

## 4. Recommended prototype order

1. **Instruments first (no perf change):** DEBUG line for the consumer gate state per filter; a mark for "Publishing
   Task returned" next to `dynfilter::accum::visible`. Needed to judge OP1, OP2, OP4.
2. **OP1 as a throwaway prototype, measured before any review.** Pass bar on this machine: q3 >= 25 ms, q8 >= 25 ms,
   suite >= 100 ms, 0 scans before visible, q4 / q10 flat. Then the PEER microbench for the bounce-excluded figure.
3. **OP2 step 0** (q8 j41 diagnosis): cheap, and may turn a dead filter into a useful one.
4. **OP4 measurement** (microbench + the 35 vs 72 G rows/s question), then a kind-policy A/B on q17 and q8.
5. **OP5** only after OP1, as one full-suite run at 8 bits.
6. OP3 and OP7 opportunistically; OP6 handed off.

## 5. Killed by measurement

| Hypothesis / lever | Why | Source |
|---|---|---|
| H01 declined or skipped attempts | 0 declines, lease or cap refusals; 46 = 46 filters at base and tip; q2 j42 and q20 j45 skipped at both (targets drained) | BASELINE 1.3, BASE_REFERENCE 2 |
| H02a domain cap | never binds: every logged domain exceeds rows, 12 of 20 unknown; q21 j18, q10 j10, q4 j9 are unique `o_orderkey`, q12 j8 1.07x | BASELINE 1.3, VERIFY_methodology #12 |
| H02 kernel-speed argument for fewer bytes | insert and probe flat from 768 MiB up; in the knee it is ~8.5 ms per suite | MB_BLOOM, VERIFY_transfer #3 |
| H03 fixed cost of the forward path vs one-shot | +0.03-0.5 ms per join, 3.2 ms worst case; tip faster for a 10M-row Bloom at G=4. (Its critical-path placement lives on as OP1) | MB_LIFECYCLE MB5 |
| H04 second stream beside the scatter | 0-0.7%: task time is the exact sum | MB_BLOOM 1.3 |
| H04 plain `add`, atomics, block layout, 1024-thread launch | 0% or 9-61% slower; kernel bound by one dirtied sector per key | MB_BLOOM 4-5 |
| H04 sort by block | at best 6% in one corner once the sort is counted, a loss elsewhere | MB_BLOOM 5 |
| Read-before-write insert | 2.4x only on x10 duplicates; TPC-H has one duplicate-heavy build (q9 j29, <= 0.6 ms); 0 to +2 ms on q21 | VERIFY_transfer #6 |
| H06 single wait / merged waits | 6-20 us per join; non-Bloom chain < 11 ms suite. (Finish off the critical path is not killed: OP1) | MB_LIFECYCLE, PROFILE 5 |
| H07 eager leases, time | begin 12 ms suite, zero-fill async at 0.7 ms/GB; no reservation wait or downgrade. Memory side untested | PROFILE 5, MB3 |
| H08 many small batches | 0.03-0.05 ms per contribution; q12 (44) 2.2 ms, q18 (36) 2.6 ms | PROFILE 5, MB2 |
| H09 publish before the final scatter | no consumer scan starts before the build site is done (276 of 276) | PROFILE 1.2 |
| H10, H11 | not reached: all filters published, no OOM or reservation retry | PROFILE 5 |
| H12 chunk size | 0-4% on PEER from 8 to 32 / 64 MiB | MB4 |
| H13 bounds | no `bounds::*` range in 17 queries; zone maps off | PROFILE 3 |
| H14 mutex across begin | begin <= 0.39 ms, one 1.84 ms outlier | PROFILE 5 |
| "Earlier publication helps" (assumed benefit of the stack) | not visible in timings on any query; q3 and q8 are slower | BASE_REFERENCE 5.4 |

## 6. Verifier corrections kept

- "DF construction costs ~45 ms (0.30%) without the bounce": 45 +- 15 ms as measured (mixed instruments); 52-60 ms
  with q21 corrected; ~295 ms if the P2P-equivalent finish is counted.
- PEER model "q21 ~30 ms, suite 64-70 ms": the rate is per target. Corrected q21 ~85-104 ms, q3 ~17-22, suite 190-215 ms.
- "All of `accum::finish` is excluded bounce": only the ingress legs are excess (HOST one source = 3.01x one PEER target).
- q21 critical insert: 49 ms on one GPU, 44 ms net (not 26); suite ~69 ms (not 46).
- q2 and q16 DF net <= 0 are real, not noise: q2 -6.5 ms (-4.4%, same at base), q16 -5 ms (-1.7%).
- "All others within noise" base vs tip: q16 +8 ms (+2.6%) is real; q5 +15 ms likely (not re-run).
- Regression cause is the synchronous finish before deposit, not "H03 fixed cost".
- Base one-shot 1248 ms vs tip 526 ms (NVTX sums) are not comparable: tip time is 1:1 wall, base time mostly is not.
- MB6 probe model does not transfer: production is 2.3-3.0x faster on 280 MiB-1.4 GiB arrays, 0.6-0.95x on 60-110 MiB.
- Probe kernels were missing from the ranking: about 4-5x the insert kernel time.
- `SIRIUS_ENABLE_TEST_OPTIONS=1` on DF-off runs is not registration-only, but its measured effect is zero.
- Iteration 1 is a second warm-up for q2, q7, q8, q10, q11, q14, q16; suite ratios do not move when it is dropped.

## 7. Unverified / open

- OP1 has no prototype. Its bound is inferred from published tables plus the base comparison; contention with the build
  CONCAT is unmeasured, and realization at base was 10-55% of the bound on q9 and q3.
- Every P2P-equivalent figure is a model: this machine has P2P only inside the pairs 0-1 and 2-3, so 3 PEER targets
  cannot be measured.
- q8 j41: why the Bloom is never probed is unknown.
- OP4 rests on kernel records only; the two exact-set probe rates have no identified cause.
- 8 bits/key downstream false-positive cost is unmeasured.
- `replicate_devices` transport is assumed HOST-staged; if not, add <= 4.7 ms (q2), 2.6 ms (q8), 2.3 ms (q20) to the
  non-excluded side.
- No ncu (permission denied): the kernel-bound diagnosis rests on differential kernels and nsys.
- Results were never validated against DuckDB CPU (tip and DF-off agree except q11 tie order).
- q5 and q9 base vs tip not re-run; q22 and q13 base figures too noisy to judge; q14 PARTITION 273 -> 370 ms in the trace
  pass with no DF is unexplained.
