# DESIGN_S6_PR: production design for S6 (early probe release, issue #1825), on top of #2570

2026-11-28. Architect document. **No GPU run, no build, no production code for this file.** Every number is quoted from
the note named next to it; arithmetic done here is marked "(computed here)". Code refs are at tip `pr2570` `ae939824`
(read with `git show`), prototypes `proto/eager-probe` f05cfd12 and `proto/eager-opt2` fdd2caec.
Inputs: `ISSUE_1818.md`, `ISSUE_1825_S6.md`, `PROTO_EAGER.md` (6-8), `VERIFY_PROTO_EAGER.md`, `EAGER_SCOUT.md`,
`COMBINED.md`, `PROTO_HYBRID_ONESHOT.md` (no verifier note exists yet; verdict so far: construction stays PARTITION-only),
`DECISION.md`, USER DECISIONS block of `OPPORTUNITIES.md`, `docs/super-sirius/{task-creator,pipeline-execution,memory-management,data-size-estimation,dynamic-filters}.md`.

Status: DESIGN, awaiting the human decisions of section 14. Checkpoint log at the end.

## 0. Summary

- What ships: when a BUILD_PROBE join's build PARTITION pipeline has closed, the join is re-requested once; if the
  release condition holds the join's task hint names the **probe producer** once instead of the build producer. Nothing
  else changes: probe batches wait in the ordinary repositories and each partition still probes only its built table.
- Release condition: `layout_stable && own_session_terminal && started_scans_terminal && memory_admitted`;
  ordinary activation (a deposited build) stays as the alternative. It is the measured "f1 + gate 2" shape
  (-497..-512 ms suite, verified -420 on 7 queries; PROTO_EAGER 6.2, VERIFY_PROTO_EAGER 0).
- Difference to the prototype (deliberate): the prototype **pushes** two requests from a callback that runs under the
  PARTITION pipeline's status lock, with raw captured references, one shot. The design is **pull / level-triggered**:
  the condition is evaluated inside the join's existing hint function on every request that passes the join, and the
  only new notification is one ordinary task request posted after the pipeline status lock is released. No callback,
  no captured reference, no subscription object, no notification that originates inside a session or channel.
- Consequence: nothing in the session / channel locking of #2570 has to change. #2570 owes two small additive items
  (section 4). Everything else lives in S6.
- Deviations from the S6 issue text, each argued by measurement (section 13): criterion 2 (gate 2 in place of the
  literal relevant-producers rule), criterion 3 (static wake wiring + level-triggered evaluation in place of a runtime
  channel subscription), criterion 6 (admission rule is real but cannot be validated under pressure on this box).
- Unmeasured in this exact form: routing the release through the join's hint (prototype schedules the probe head
  directly), posting it after the lock (prototype: inside), and the real admission rule. Gate G1 (section 11) decides;
  a contingency variant is given in 5.5.

## 1. Vocabulary (add to GLOSSARY)

| Term | Meaning |
|---|---|
| Early Probe Release | One-time nomination of a join's probe producer before any build batch of that join is deposited |
| Layout Stable | The join's build PARTITION pipeline has closed (partition count, placement and per-partition build bytes are final) |
| Started Scans of a join | Scans that nominating the join's probe producer can start before any lower join releases its own probe side (5.3) |
| Build-Progress Demand | Per GPU: bytes the join's build still needs there until every hash table is built (6) |
| Closure Wake | A task request for a fixed operator, posted when a pipeline closes, after its status lock is released (5.2) |
| Ordinary Activation | Today's path: the probe producer is named once a build batch is deposited (`select_build_probe_action`) |

## 2. Release condition (exact)

For join J, evaluated only while J's hint would answer `wait_for_build` (no schedulable build or probe work):

```
release(J) =  eligible(J)                                   -- static, decided at bind
           && layout_stable(J)                              -- build PARTITION pipeline finished
           && own_session_terminal(J)                       -- J's publication session Terminal (or J has no plan)
           && for every s in started_scans(J):
                  channel(s) is Terminal  ||  s has no channel  ||  pipeline(s) finished
           && memory_admitted(J)                            -- section 6
```

Probe nomination overall: `ordinary_activation(J) || release(J)` (S6 criterion 1). Hash probing is untouched.

`eligible(J)` (all static at bind time, `bind_partition_inputs`):
- hash join with a bound build CONCAT and `!probe_drives`;
- not MARK, not right-family (those have their own ordering: empty-build path, BUILD_FILTER);
- internal mode allows it (section 9): `all_joins`, or `filter_joins` and J's publish plan is enabled.

Runtime exclusion needs no flag: a join that negotiated STANDARD / MIXED never reaches `wait_for_build` in the
BUILD_PROBE branch, and an empty build has no slots (`select_build_probe_action` returns `none`).

Why each atom:
- `layout_stable`: the probe PARTITION needs the resolved exchange; the admission rule needs final build bytes.
- `own_session_terminal`: keeps the tip invariant "J's probe side never starts before J's filters are published or
  declined". It is checked, not inferred from the #2570 contract, so a broken contract costs the gain, never safety.
- `started_scans_terminal`: the user's rule "a scan targeted by several filters waits for ALL its filters", checked per
  started scan (prototype gate 2). 0 of 56 joins held on TPC-H, 0 of 82 activations on the 37 shapes (VERIFY 0, c1).
- `memory_admitted`: S6 criterion 6.

Evaluation order: cheapest first (atomics), admission last (reads repository sizes). No atom takes a session or
channel mutex (section 4, C2).

## 3. State machine (per join, inside the module of section 5)

```
            bind: not eligible
  UNBOUND ---------------------> INELIGIBLE                       (terminal; tip behaviour)
     | bind: eligible
     v
   ARMED --(hint pass in wait_for_build, release(J) true)--> RELEASED   (terminal; probe producer named once)
     |
     +--(hint pass with any deposited build / ordinary probe nomination / operator finalize)--> CLOSED (terminal)
```

- ARMED is memoryless: a pass that finds the condition false changes nothing except the recorded last verdict
  (`layout_pending | session_pending | filters_pending | memory_refused`), used only for the log line.
- RELEASED and CLOSED are both "tip behaviour from here on". The latch is written under the join's `op_state_mutex`
  (already held by `get_next_task_hint`), so exactly one pass can release.
- Passes that re-evaluate an ARMED join (this is the whole re-evaluation story, S6 criterion 3):
  1. the Closure Wake of J's own build PARTITION pipeline (layout + own session);
  2. the Closure Wake of the construction site of every other producer on a started scan's channel (5.4);
  3. every ordinary request that walks through J (upper join's chain, look-ahead);
  4. J being scheduled as output consumer when a build CONCAT task completes (the "normal deposit" trigger): either
     the deposit opens ordinary activation, or other partitions are still pending and admission is re-read.
- Missed wake-up: atoms 2-4 of the condition are monotone (pipeline finished, session Terminal, channel Terminal after
  registration freeze). Each rising edge is followed by a request posted **after** the state is visible (closure wake
  is posted after `finalize_operator` returned and the status lock was released). Memory is not monotone and has no
  edge notification by design (section 6.4); its fallback is ordinary activation.

## 4. Contract with #2570: what #2570 must provide, what stays in S6

### 4.1 #2570 must provide (both additive, no behaviour change)

| # | Item | Today | Needed |
|---|---|---|---|
| C1 | "Construction-site pipeline finalize => the join's session is Terminal, for every layout and outcome" | `D_ASSERT` only, `src/op/dynamic_filter/hash_join_partition_observer.cpp:60-65`; BUILD_FILTER site analogous | One sentence in `docs/super-sirius/dynamic-filters.md` (lifecycle section) + a release-build unit test over: single partition, multi partition, empty build (no task ran), declined inventory, declined admission, failed publication, cancelled, plan disabled by `restrict_replicas_to`, cross-rank merge without a filter (`finish_input` merge branch). Test file `test/cpp/operator/test_dynamic_filter_publisher.cpp` |
| C2 | Cheap `terminal()` | `sirius_dynamic_filter_set` exposes it only through `snapshot()` (channel mutex + copy of the entries); `dynamic_filter_publication_session::terminal()` takes the session mutex (`dynamic_filter_publisher.cpp:1089`) | `[[nodiscard]] bool sirius_dynamic_filter_set::terminal() const noexcept` and a lock-free session `terminal()`: an `std::atomic<bool>` written once under the existing mutex at the transition (channel: frozen and `completed_producers == producers.size()`; session: `phase::TERMINAL`), read with `memory_order_acquire`. Monotone. `snapshot().terminal()` keeps its meaning |

In S6's use C1 is a **performance** contract (if violated the join is never released and runs as on the tip) and C2 is
a lock-ordering convenience (the reads happen under the join's `op_state_mutex`).

### 4.2 Not needed in #2570 (the prototype / issue text suggested them)

- No terminal-notification on the channel or the session; `post_unlock_actions` keeps carrying key lines only.
- No change to `finish_input`, `publish_if_final`, the observer, the certifier, or any lock.
- No deferred publication (option 2 is killed: no gain, 12 files; PROTO_EAGER 6.8).

### 4.3 If #2570 is frozen

C1 + C2 become commit 1 of the S6 PR, unchanged in content. Nothing else in the design depends on where they land.

### 4.4 Lives in S6

Closure Wake hook (operator + pipeline), the `early_probe_release` module, the hint redirect in the join, the started-
scans walk, the admission rule, wake wiring in `bind_partition_inputs` + one post-planning pass, internal mode,
observability, docs, tests.

## 5. Module boundaries

### 5.1 `early_probe_release` (new, `src/op/early_probe_release.{hpp,cpp}`): the deep module

Owns: eligibility, the latch, gathering the atoms, the last verdict, the one log line. Hides: the walk, the admission
arithmetic, which objects are read. The join sees three calls.

```cpp
namespace sirius::op {

enum class early_probe_mode : std::uint8_t { off, filter_joins, all_joins };   // internal, section 9

enum class release_verdict : std::uint8_t {
  open, ineligible, layout_pending, session_pending, filters_pending, memory_refused };

/// Policy, pure. Mechanism never decides.
struct release_inputs {
  bool        eligible;
  bool        layout_stable;
  bool        own_session_terminal;
  std::size_t pending_started_channels;
  bool        memory_admitted;          // evaluated lazily by the caller: only when all above pass
};
[[nodiscard]] constexpr release_verdict decide_release(release_inputs const&) noexcept;

/// Policy, pure. One entry per GPU of the query's admitted set.
struct gpu_headroom { std::size_t used_bytes; std::size_t stop_threshold_bytes; };
template <std::ranges::input_range H, std::ranges::input_range D>
  requires std::convertible_to<std::ranges::range_reference_t<H>, gpu_headroom const&> &&
           std::convertible_to<std::ranges::range_reference_t<D>, std::size_t>
[[nodiscard]] constexpr bool admits_build_progress(H&& headroom, D&& demand_bytes) noexcept;

/// Mechanism. One per hash join, owned by the join. Not thread safe: every call is made under the
/// join's op_state_mutex except bind(), which runs before execution.
class early_probe_release final {
 public:
  struct wiring;                                   // build PARTITION, build CONCAT, session, started scans,
                                                   // admitted GPU spaces + stop thresholds, mode
  void bind(wiring) noexcept;                      // once; absent => INELIGIBLE
  [[nodiscard]] bool claim() noexcept;             // true exactly once, when release(J) first holds
  void close() noexcept;                           // ordinary activation took over / operator finalized
  [[nodiscard]] release_verdict last_verdict() const noexcept;
};

/// Started scans of a probe head (5.3). Free function, used by bind and by the wake wiring.
[[nodiscard]] std::vector<scan::sirius_gpu_scan_operator*> started_scans(sirius_physical_operator& probe_head);

}  // namespace sirius::op
```

- Templates / concepts are used where a test wants to inject values (the two pure policy functions take ranges, so
  unit tests pass arrays and production passes views over memory spaces). No inheritance, no `std::function`.
- `claim()` and `close()` are `noexcept`: any internal failure is logged once and reported as "not released".

### 5.2 Closure Wake (general, small): `sirius_physical_operator` + `sirius_pipeline`

```cpp
// sirius_physical_operator.hpp, next to add_finalize_listener
//! Requests @p target once this operator's pipeline has closed. Unlike a finalize listener this is data, not
//! code: the pipeline posts an ordinary task request after it has released its status lock. Register while planning.
void request_on_pipeline_close(sirius_physical_operator& target);
[[nodiscard]] std::span<sirius_physical_operator* const> pipeline_close_requests() const noexcept;
```

`sirius_pipeline::update_pipeline_status` (`src/pipeline/sirius_pipeline.cpp:367-440`): on `just_closed`, after the
`_status_mutex` scope, after `notify_downstream_pipelines`, post `creator->schedule(target)` for every target of every
operator of the pipeline, deduplicated, skipping `target->finalized`, inside the same try / `report_fatal_error` shape
`notify_downstream_pipelines` uses. This is the **only** notification S6 adds. It runs on the code path and under the
lifetime rules that already schedule output consumers a few lines above.

Finalize listeners stay as they are (used by `src/planner/distributed_plan.cpp:638`); S6 adds none.

### 5.3 Started-scans walk (inside the module)

From the probe head, over `port->src_pipeline->get_operators()[0]`:
- GPU_SCAN: collect.
- hash join that is build-first (`!probe_drives`): follow the **build** port only (its probe side is released later by
  its own condition or by ordinary activation).
- every other operator, including probe-driven hash joins, nested-loop, delim, dense-count, UNION: follow **all** ports.
- depth guard as in the prototype.

Rule for extending it: over-approximation is always safe (a scan that is checked although it starts later can only hold
the join, which then runs as on the tip); under-approximation is never allowed. This fixes the prototype's blind spot
(it followed only the build port of every hash join, including probe-driven ones; PROTO_EAGER 6.9 "treats every
non-hash-join operator as transparent").
Computed once at bind (static). `pipeline(s) finished` is read at evaluation time.

### 5.4 Wake wiring

- `bind_partition_inputs` (`src/op/partition/consumer_partition_policy.cpp`): `build.request_on_pipeline_close(join)`
  when the join is eligible. Replaces the prototype's `add_finalize_listener`.
- One pass after planning, when pipelines and channels exist and before `start_query` (developer: place it where the
  dynamic-filter consumers are attached to scans; `early_probe_release::bind` needs `port->src_pipeline`): for every
  eligible join J compute `started_scans(J)`; for every join U != J whose publish plan targets a channel of a started
  scan of J, `site(U).request_on_pipeline_close(J)` where `site(U)` is U's construction site operator (build PARTITION
  or BUILD_FILTER). By C1 the site's closure is not earlier than U's session Terminal, so this is the "last producer
  terminal" notification of S6 criterion 3, delivered without touching a session or channel lock. Cost: a loop over
  joins x targets at plan time. On TPC-H and the 37 shapes it never changes an outcome (0 held joins).

### 5.5 The join (`src/op/sirius_physical_hash_join.{hpp,cpp}`)

`get_next_task_hint`, BUILD_PROBE branch (`:969-1000`), only this:

```
case wait_for_build:
    if (!probe_finished && _early_probe.claim())  -> WAITING_FOR_INPUT_DATA, producer = probe head   // once
    else                                           -> as today (build producer)
case schedule_build / schedule_probe / wait_for_probe:
    _early_probe.close();                                                                            // then as today
```

`select_build_probe_action` (pure, unit tested) is unchanged. `on_finalize_operator` calls `close()`.
The prototype's `activate_probe_after_build_partition`, `_eager_probe_activated`, env reads and the throttle are not carried over.

Why the build still progresses after the redirect (invariant I1): after the build PARTITION pipeline has closed, the
build CONCAT is requested by the closure path itself (`notify_downstream_pipelines`, or the executor epilogue's
captured consumers when `original_pipeline`), and a CONCAT whose source finished creates all its tasks from one
request. J's hint is not what drives the CONCAT in that state. The developer must confirm the epilogue branch and pin
I1 with a unit test (section 10, T7).

Contingency variant B (only if gate G1 shows the hint route loses time against the prototype): keep everything, but
make the Closure Wake targets the build CONCAT and the probe head, with `claim()` called from the closure path under
`op_state_mutex` and the two requests posted after it is released. Same module, same condition; costs the level-
triggered re-evaluation on passes 3-4 (they would need the hint hook anyway). Not recommended up front.

## 6. Memory admission

### 6.1 What must be protected

Early probe output is ordinary repository data: its tasks take reservations, its idle batches are the first spill
candidates of the downgrade executor (memory-management.md "Downgrade Request Pattern"). The thing S6 must not do is
let that output occupy the bytes the **build** still needs between Layout Stable and the last hash table (S6: "without
consuming build-progress capacity"). So the rule sets the build's demand aside and requires spare room on top.

### 6.2 Rule

For every GPU g of the query's admitted set (`active_gpu_ids`):

```
used(g)   = max_memory(g) - available_memory(g)                 -- live reservations + resident data
stop(g)   = downgrade_stop_fraction(g) * max_memory(g)           -- lower edge of the spill hysteresis band
demand(g) = sum over build partitions p placed on g of
              landing(p, g)            -- bytes of p not yet resident on g (the CONCAT move)
            + 2 * bytes(p)             -- concat output + hash table / working set
            (unpinned placement: charge p to every GPU)

memory_admitted(J)  <=>  for all g:  used(g) + demand(g) <= stop(g)
```

- Reading: after the whole build has landed and built, every GPU is still below the point where spilling **stops**;
  the full band `[stop, trigger]` (20% of capacity with the defaults 0.6 / 0.8, configuration.md) is free for early
  probe output before the engine's ordinary backpressure even starts. Beyond that the existing rules apply unchanged,
  exactly as they do on the tip for probe output buffered while later partitions build.
- Inputs are all available without a new lock: per-partition bytes and current residency from the build CONCAT's input
  repository (final at Layout Stable), placement from the join, `get_max_memory` / `get_available_memory` from
  `cucascade::memory::memory_space`. The stop threshold is private in cuCascade
  (`_stop_downgrading_memory_threshold`): take the fraction from Sirius's own configuration of that space; **no
  cuCascade change**.
- The factor 2 is the one tunable; it is an internal constant, not a knob. It is conservative in the common case and
  can be too small only for very narrow builds (hash table larger than the table); those builds are also the small ones.
- The same rule applies to joins with and without a filter plan (S6: "a terminal no-filter result does not itself
  justify early work" - it does not; memory does). No probe-size estimate is used: at Layout Stable the probe pipelines
  have no measured ratios, so `estimate_port_total_input_bytes` (data-size-estimation.md) would be `nullopt` or
  planner-derived, and a rule that needs it would never admit.

### 6.3 Expected behaviour on the benchmark machine (computed here from the notes, not run)

Lowest free memory at any activation: 74.7 GiB of 95.6 (PROTO_EAGER 6.6, reproduced by the verifier) => used <= 20.9 GiB.
With the default stop fraction 0.6 of the 85.5 GiB limit = 51.3 GiB, a join is admitted while its demand on the tightest
GPU is <= about 30 GiB. Expectation: 0 refusals on the 56 TPC-H joins. This is an expectation; gate G3 checks it. If the
config's fractions differ the margin changes, not the rule.

### 6.4 Re-evaluation

Admission is re-read on every pass of section 3 while the join is ARMED. There is no timer and no memory-release
subscription: the next guaranteed pass is the first build deposit, which opens ordinary activation anyway, so a join
refused for memory behaves as on the tip (the prototype's "never admit" arm cost -3.7 ms on q3, results identical;
PROTO_EAGER 6.6). A release is not revocable; after it, ordinary reservation / downgrade rules govern.

### 6.5 What stays unvalidated

Pressure: nobody has run eager scans near a memory limit (VERIFY 3). The unit tests of section 10 cover the rule and
the fallback; an engine-level pressure run needs a small-memory configuration, which the campaign rules forbid for the
benchmark config (question Q6).

## 7. Notification outside locks (S6 criterion 4)

| Notification | Origin | Locks held when posted | Target lifetime |
|---|---|---|---|
| Closure Wake (new, the only one) | `sirius_pipeline::update_pipeline_status`, after the `_status_mutex` scope | none | `task_creator::schedule`: submission gate refuses a retiring query, queued requests are dropped by `discard_pending_tasks`, a processed request holds a work lease; operators are destroyed only after `retire_query_work` waited for the leases (query-lifecycle.md). Same guarantees as the consumer scheduling next to it |
| Release itself | return value of `get_next_task_hint` | `op_state_mutex`, but nothing is sent: the creator follows the returned producer after the hint returns | n/a |

No notification originates from `dynamic_filter_publication_session`, `sirius_dynamic_filter_set`, the certifier or
PARTITION's first-pop lock, so the "session begin under PARTITION's first-pop lock" case of the issue cannot arise.
No callback is stored anywhere; there is nothing to invalidate before teardown.
Lock order added: `op_state_mutex` -> (atomics only) and `op_state_mutex` -> repository size reads, which
`snapshot_build_probe_slots` already does on the same path.

## 8. Hang / deadlock analysis

General argument: S6 adds no wait. It adds (a) one extra task request per pipeline closure and (b) a one-time change of
which producer a hint names, valid only in a state where the build producer is already requested by the closure path
(I1). Every "held" outcome is the tip's behaviour.

| Case | What happens | Why no hang |
|---|---|---|
| No filter plan | `own_session_terminal` true by definition; released under the same admission rule (mode `all_joins`) | as below |
| Declined / skipped / failed / cancelled / plan-dropped filters | session Terminal without filters (C1); released, scans run unfiltered as on the tip | Terminal is checked, filter presence is not |
| C1 violated (bug) | `session_pending` forever | ordinary activation on the first deposit |
| Empty build | no PARTITION task, no slots, hint returns `none` / the MARK empty path; wake request is a no-op | tip path |
| Zero-row but non-empty build | BUILD_PROBE negotiated, released like any other; probe waits for the (empty) table | tip waiting rules; needs a test (T9), untested by the prototype |
| Probe finishes before any build lands | probe batches wait in the probe CONCAT / probe port; `wait_for_build` continues to name the build producer | measured on q12, q16, q7, q3 j15, q8 j41, q5 (PROTO_EAGER 8.5); `!probe_finished` guards the redirect |
| Stacked probes (J above L on J's probe path) | J releases; the request reaches L, which answers `wait_for_build` and starts its build subtree (those scans are in `started_scans(J)`); L releases at its own closure | `started_scans(J)` excludes scans behind L's probe port, so J never waits for L (the gate-1 cycle: 4 joins held forever, 35 ms; VERIFY c2) |
| Producer above J not Terminal at J's closure (look-ahead strategy, STANDARD-mode upper join, any shape not seen) | `filters_pending`; re-evaluated at that producer's site closure (5.4) | if it never opens: ordinary activation |
| Producer the walk cannot place (probe-driven lower join, non-join producer) | over-approximation: checked; if its channel never becomes Terminal before J's build lands, J is not released | ordinary activation |
| Filter target in the build subtree of a lower join | that scan is in `started_scans(J)` and is checked | covered; test T6 |
| Multi-filter scan | every producer on its channel is either J, above J (Terminal by the upper join's own release / activation), or holds J | user's rule holds by check, not by induction |
| Interrupt / cancellation / teardown with a queued wake | request dropped by the submission gate / `discard_pending_tasks`; a processed one holds a work lease | no stored callback, no raw capture (prototype risk 1, VERIFY 4) |
| Query error in any task | completion handler fails the query; S6 state dies with the operators | no S6 wait exists |
| Exception inside the wake post | same try / `report_fatal_error` as `notify_downstream_pipelines` | fails the query, cannot strand it |
| Exception inside `claim()` | `noexcept`; logged once; not released | ordinary activation |
| OOM on a build task that lost memory to early scans | ordinary downgrade-on-demand: idle probe batches are spilled first, the build task retries | existing rules; the admission rule exists to make this rare; unvalidated under pressure (6.5) |
| Two requests racing through J | latch under `op_state_mutex`; exactly one redirect | - |
| Throttle | not in S6 | the prototype's blocking wait and its leak (VERIFY 4.3) are not carried over |

Not covered by reasoning alone, must be tested: I1 on the `original_pipeline` branch, zero-row build, two queries at
once (per-query state only, but untested), compressed pinned scans on the non-TPC-H shapes.

## 9. Internal mode, observability, docs

- Mode: `early_probe_mode { off, filter_joins, all_joins }`, default `all_joins`. Not in YAML, not a public knob.
  Settable by tests directly (build context) and, for benchmark ablation, through the existing
  `SIRIUS_ENABLE_TEST_OPTIONS` mechanism (`src/planner/sirius_physical_plan_generator.cpp:2145`,
  `src/sirius_context.cpp:1781`). Read once at plan generation and stored in the wiring. No `std::getenv` in operators.
- Log: one INFO line per eligible join when it leaves ARMED:
  `[early_probe] join N {released|closed} verdict=<last verdict> filter_plan=<bool> started_scans=<n> pending_channels=<n> demand_gib=<max over g> headroom_gib=<min over g>`.
  This is what gates G3 / G5 parse (the prototype's `[eager]` line plays that role in `runs/veager/bin/cmp_logs.py`).
- Stats: two counters next to the dynamic-filter stats (released, closed-by-verdict). Quent: nothing new; publication
  and task-creation events already give the ordering check (`runs/profile/bin/quent_df.py`).
- Docs (S6 criterion 8): `task-creator.md` (HASH_JOIN BUILD_PROBE section: the redirect; "Scan Scheduling Strategy":
  second activation source), `pipeline-execution.md` (Closure Wake), `dynamic-filters.md` (ordering: started scans see
  Terminal channels; C1 if not already there), `memory-management.md` (admission rule), GLOSSARY (section 1).

## 10. Test plan

Unit, no GPU (pure policy; `test/cpp/operator/test_early_probe_release.cpp`, new):
- T1 `decide_release`: full truth table; verdict precedence layout > session > filters > memory.
- T2 `admits_build_progress`: boundary (equal to stop admits), one tight GPU refuses, empty demand admits, unpinned
  placement charged everywhere, overflow-safe addition.
- T3 `started_scans` on hand-built operator graphs: linear chain, stacked build-first joins (probe port not followed),
  probe-driven lower join (both followed), UNION / nested-loop / delim (all ports), filter target under a lower join's
  build, depth guard.
- T4 `early_probe_release` latch: releases once; `close()` before release wins; unbound = ineligible; `noexcept` path.

Unit, engine fixtures (`test/cpp/operator/test_build_probe_scheduling.cpp`, `test/cpp/creator/test_task_creator.cpp`,
`test/cpp/operator/test_dynamic_filter_publisher.cpp`; real build / CONCAT / probe graph where the fixture allows):
- T5 both notification orders: layout closes then last producer Terminal, and the reverse; exactly one release each.
- T6 unrelated producer on a started scan's channel pending -> held; its site closes -> released (5.4 wiring).
  Producer in the probe subtree does not hold (stacked probes).
- T7 I1: after the redirect the build CONCAT still receives its request and all partitions build, on both
  `original_pipeline` branches.
- T8 zero-filter terminals: all skipped, declined, failed, cancelled, plan dropped -> released without a filter;
  no plan + mode `filter_joins` -> not released; mode `off` -> hint sequence identical to the tip.
- T9 zero-row non-empty build; empty build (wake is a no-op); probe finishing before any build lands.
- T10 admission refused -> ordinary activation, results equal; refused then admitted on a later pass (multi-partition
  build, second pass at a deposit).
- T11 teardown: wake queued, query retired -> request dropped, no use after free (run under ASan); interrupt between
  closure and wake.
- T12 missed wake-up: closure and a concurrent ordinary request through J in both interleavings (TSan-friendly, loop).
- T13 exclusions: MARK, right-family, STANDARD / MIXED, nested-loop, delim -> INELIGIBLE or never in `wait_for_build`.
- T14 C1 / C2 tests (4.1), in whichever PR carries them.
- T15 pressure (GPU, test config with a small GPU capacity under `test/cpp/config`, not the benchmark config): early
  probe output buffered while the build moves; expect either refusal or completion with downgrade, no OOM failure,
  results equal. Needs Q6.

Engine, GPU, the verifier's suite (`$R/runs/veager/bin/shapes.py` + `shapes.sh`, 37 statements x 2, watchdog 240 s,
then 15 `interrupt()` calls 30-400 ms into a join query each followed by a normal query; oracle DuckDB CPU):
join without a filter plan, declined / near-full-domain filter, empty build (inner, left), one-row build, empty probe,
two and three filters on one scan, 4-deep stacked probes, self-joins, LIMIT 10 / LIMIT 0 / plain LIMIT / LIMIT over a
two-filter scan, top-N, semi, anti, MARK, left / right / full outer, filter target under a lower join, UNION ALL on the
probe side, CTE used twice, bushy join, residual inequality, aggregate on the build side, correlated subqueries
(delim), window on the probe side, two-column key, left-join chain with NULL keys, string key.
Arms: tip, `all_joins`, `filter_joins`, `off`. Pass = 0 hangs, rc 0, results equal to the tip and to DuckDB CPU.
Add to the suite: the same 37 on **pinned / compressed** scans (not exercised so far), and two concurrent queries.

Repository gates: `pixi run make test`, `[multi_gpu]`, `pixi run pre-commit run -a`; ASan + TSan on T4-T12.

## 11. Benchmark gates (recipe: BASELINE_SUITE 0-1; fresh process per query, 1 warm + 5 hot, drop first hot, ABBA, medians)

| Gate | Pass | Reference |
|---|---|---|
| G1 suite, `all_joins` vs tip, same session | <= -450 ms; no query worse than max(3%, 10 ms) | prototype -497..-512 (PROTO_EAGER 6.2); verifier 7 queries -420 |
| G2 suite, `filter_joins` vs tip | <= -260 ms | -288 (COMBINED 5), -324 mixed sessions |
| G3 release log | 56 joins released per pass; 0 `filters_pending`, 0 `memory_refused`, 0 `session_pending` at close | VERIFY c1 |
| G4 binary control, `off` vs tip | within +-30 ms suite | verifier -4.4 over 5 queries |
| G5 ordering | 0 consumer scan tasks created before any publication targeting the scan (Quent); rows in / out of every DYNAMIC_FILTER operator equal to the tip (TRACE pass, `cmp_logs.py`) | PROTO_EAGER 6.7, VERIFY 2 |
| G6 results | 22 of 22 identical to the tip (q11 after sorting) | 6.7 |
| G7 memory | resident batches <= tip + 3.5 GiB on the worst GPU (q18); 0 OOM / retry / downgrade lines | VERIFY 3 |
| G8 shapes | section 10 suite: 0 hangs, equal to DuckDB CPU | VERIFY 4 |

If G1 fails but G3-G8 pass: build contingency variant B (5.5) and re-run G1 before anything else.
After S6 passes alone: measure the missing combined arm S + X + `all_joins` (DECISION 5.6; estimate about -970 ms, not measured).

## 12. Ordered implementation plan (developer agent; one commit per step, build + unit tests green at each)

0. Worktree from `pr2570` (`git submodule update --init --recursive`), branch `prod/s6-early-probe`. Read
   `docs/super-sirius/` (README order), this file, `ISSUE_1825_S6.md`. Run `/module-context` for cucascade memory
   spaces and the task creator.
1. C2: lock-free `terminal()` on `sirius_dynamic_filter_set` and on the session (+ tests). [#2570 or S6 commit 1, Q4]
2. C1: doc sentence + release-build test matrix; keep the `D_ASSERT`. [#2570 or S6 commit 1, Q4]
3. Closure Wake: `request_on_pipeline_close` on the operator, posting in `update_pipeline_status` after the lock;
   unit test that the request is posted exactly once per closure, after finalize, never under `_status_mutex`,
   dropped for a retired query (T11).
4. Pure policy: `decide_release`, `admits_build_progress`, `started_scans` + T1-T3.
5. `early_probe_release` (latch, wiring, verdict, log line) + T4. Admission input gathering from the build CONCAT's
   input repository, placement and memory spaces; stop fraction from Sirius configuration.
6. Join: member, the two-line hint change, `close()` on finalize; `bind_partition_inputs` wake registration;
   mode plumbed from plan generation (default `off` in this commit). T5, T7-T9, T12, T13.
7. Post-planning pass: `bind` + cross-producer wake wiring (5.4). T6.
8. Admission live + T10; T15 if Q6 allows.
9. Mode `filter_joins` on by default -> run G2, G5, G6 on the filter queries. Commit.
10. Mode `all_joins` on by default (separate commit: the general scheduler change with no DF dependency) -> G1, G3-G8.
11. Docs + GLOSSARY + stats. `pre-commit run -a`.
12. Reviewer agent: liveness, lock / notification ordering, teardown, memory progress (S6 delivery list). Fix findings
    by re-deriving the minimal change. Record everything in `$R/notes/IMPL_S6_PR.md` with a checkpoint log;
    `bash /home/ubuntu/dfperf-durable/backup.sh` after each step.

Expected size (estimate): about +450 source lines (module ~250, pipeline / operator hook ~40, join ~30, wiring ~60,
C1 / C2 ~40, mode + stats ~30), about +700 test lines, docs. Not carried over from `proto/eager-opt2`: throttle
(`src/op/eager_probe_throttle.hpp`, `gpu_pipeline_task` hook), option 2, gates 0 / 1, env switches, the cuCascade patch.

## 13. S6 acceptance criteria: status under this design

| # | Criterion (short) | Design | Deviation |
|---|---|---|---|
| 1 | keep normal deposit readiness as the alternative | yes, by construction (the redirect exists only in `wait_for_build`) | none |
| 2 | layout from PARTITION completion; terminal rights of all relevant endpoint producers incl. empty / skipped / failed / dropped | layout = pipeline closure; Terminal checked on J's session and on the channel of every **started** scan; presence of a filter is never used | **Yes.** "Relevant endpoints" = the endpoints the release starts, not every channel J publishes to. Measured: the literal rule can never open for stacked probes (4 joins held every iteration, 35 ms; VERIFY c2), the started-scans rule never held any of 56 joins. Issue text needs the update (Q2) |
| 3 | subscribe-and-recheck, re-evaluate on layout, last-producer terminal, normal deposits; coalesce through task requests | level-triggered evaluation in the hint on every pass; passes guaranteed by static Closure Wakes (own site, other producers' sites) and by the ordinary consumer scheduling at deposits; all are task requests | **In mechanism only.** No runtime subscription exists; the last-producer notification arrives at that producer's site closure (>= its Terminal instant, by C1) instead of at the publication instant. Never exercised by TPC-H or the 37 shapes (0 held), so unmeasured either way (Q3) |
| 4 | notify outside locks; weak targets | one notification, posted after the status lock is released; targets are task requests under the creator's lifetime rules; none from session / channel / first-pop | none |
| 5 | preserve progress for absent sources, unsupported shapes, probe-subtree producers; no new wait or cycle | section 8; throttle excluded | none |
| 6 | admit through existing reservation / backpressure without consuming build-progress capacity; no-filter result is not a justification | rule of 6.2 for every join; refusal = ordinary activation | **Validation gap, not a design gap**: pressure is untested on this machine (Q6). About 190 of the 500 ms come from joins without a filter (q9, q18, q14, q22), which rest entirely on this rule |
| 7 | validation list | section 10 (T5-T15 + shapes) | pressure test depends on Q6 |
| 8 | docs, observability, internal ablation, no public knob | section 9 | none |

## 14. Open questions for the human (each with a recommendation)

| # | Question | Recommendation |
|---|---|---|
| Q1 | Scope: release joins **without** a filter plan too (190 of ~500 ms; the only place memory grows, q18 +2.1..3.5 GiB)? | Yes, same admission rule, as its own commit (step 10) with mode `filter_joins` as the ablation. If pressure validation (Q6) is refused, ship `filter_joins` as default and keep `all_joins` internal |
| Q2 | Accept the started-scans rule in place of the issue's literal relevant-producers rule and update the #1825 text (criterion 2)? | Yes. The literal rule is a measured loss and cannot open for stacked probes |
| Q3 | Accept static Closure Wakes + evaluation in the hint as the "equivalent level-triggered behaviour" of criterion 3 (no channel subscription, no change to #2570's locks)? | Yes. One notification source that is already outside every lock; smallest change to #2570 |
| Q4 | Where do C1 / C2 land: appended to #2570, or commit 1 of S6? | #2570 if it is reopened anyway for the shadow-rule rebase (DECISION 4.1); otherwise S6 commit 1 |
| Q5 | Admission constants: threshold = downgrade **stop** edge, demand factor 2 (+ landing bytes) | Accept as internal constants; revisit only if G3 shows refusals or T15 shows spills |
| Q6 | May an engine-level pressure run use a dedicated small-memory **test** config (not `specific_config.yaml`, no benchmark claim)? | Yes; without it criterion 6 stays validated by unit tests only |
| Q7 | Throttle (K=1, -80..-100 ms more): internal default-off switch in S6, or nothing? | Nothing in S6. It is a transfer symptom with a blocking wait and a known pairing defect; the transfer follow-up owns the fix |
| Q8 | Active memory re-evaluation (timer or memory-release subscription) for refused joins? | No. The first deposit is the next event and opens ordinary activation; a refused join costs about what the tip costs |
| Q9 | Keep MARK, right-family, STANDARD / MIXED, nested-loop and delim joins excluded? | Yes for S6; none was measured |
| Q10 | Mode control through `SIRIUS_ENABLE_TEST_OPTIONS` (default `all_joins`, no YAML)? | Yes; matches "internal ablation, no public knob" |
| Q11 | Landing order vs the transfer PR (user: independent). The combined `all_joins` number is unmeasured | Land in either order; measure S + X + `all_joins` once both production branches exist |
| Q12 | Shadow rule interaction: its gain depends on publication order (31 ms margin on q21 today, DECISION 2); S6 changes when scans and later publications run | Re-run the shadow-rule check (q21 j18, q8 j41 still shadowed) inside G1's session if the shadow branches are applied first |

## 15. Risks

- The hint route and the post-lock wake are not what was measured (prototype: direct schedule under the lock). Expected
  difference: microseconds (CONCAT starts 0.1 ms after publication today, VERIFY 5). G1 decides; variant B is the fallback.
- Admission is validated by arithmetic and unit tests only until Q6 is answered.
- I1 rests on reading `sirius_pipeline.cpp` and `gpu_pipeline_executor.cpp:400`; T7 must pin it.
- The walk's operator table (5.3) must be kept conservative when new multi-input operators are added: default "all
  ports" makes the safe choice the default.
- One machine, P2P only inside pairs; the gain is the reclaimed slack between PARTITION finalize and the first build
  deposit and shrinks as the transfer PR shortens the build move.

## Checkpoint log (chronological; newest last)

- 2026-11-28: file written from the notes and from `git show` of `pr2570`, `proto/eager-probe`, `proto/eager-opt2`.
  No GPU process, no build, no code. Next: human answers to section 14, then developer on section 12 from step 0
  (notes file `IMPL_S6_PR.md`).
