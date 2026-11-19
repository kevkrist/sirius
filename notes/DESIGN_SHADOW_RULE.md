# DESIGN_SHADOW_RULE: do not construct a Membership Filter no Consumer would select

2026-11-19. Architect spec for the PR stack #2569 (S5B1) -> #2570 (S5B2). No production code, nothing built or run.
Inputs: PROTO_OP2.md, VERIFY_PROTO_OP2.md, pr_split.md, construction.md, `docs/super-sirius/dynamic-filters.md`,
`src/op/dynamic_filter/GLOSSARY.md`, code at pr2569 `a2d6bd5a` / pr2570 `ae939824` (read-only, `R/repo`), prototype diff
`proto/op2-dead-filters`. Line refs are at pr2570 unless marked.

Reading done for this role: see section 11. Wording follows GLOSSARY.md ("endpoint" is an Avoid word there: this spec says
Consumer; code identifiers that still say `endpoint` are quoted as they are).

## 1. Problem and decision

q8 j41 (174 MiB Bloom) and q21 j18 (1394 MiB Bloom) are constructed, merged, replicated, and then never applied: Filtered
Decode carries one probe (the stronger IN-list) and the Consumer's negligible-share rule skips the Bloom on every batch.
Cost here 335.6 ms (2.18% of the suite, verified ABBA x3), 83-152 ms P2P-equivalent (model). With S6 (a scan waits for all its
Producers) the dead construction sits fully on the critical path.

Decision: a key whose Membership Filter no Consumer would select, judged with the Consumer's own static rule against the
filters already on the key's Channels, gets no Membership Filter in its Construction Plan. One definition of that rule,
shared by the Consumer and the planner. Judged twice with the same function: at `begin` (saves everything) and just
before `finish` (saves merge + replication when the stronger filter arrived during Construction).

## 2. Module boundary

Three pieces, each with one owner of its knowledge.

### 2.1 `src/op/dynamic_filter/static_filter_selection.{hpp,cpp}` (new, #2569): the Consumer's Static Selection

Owns everything a Consumer decides about a Membership Filter before measuring it: the negligible-share threshold and
formula, Filtered Decode's preference order (kind rank, then key count, then publication order), and how those facts are
read off a filter object. Today these live in the anonymous namespace of `src/op/scan/dynamic_filter_merge.cpp`
(`k_negligible_removed_share` :93, loop :605-623, `static_selectivity` / `static_selectivity_of` :415-440). They move here;
`src/op/scan` already depends on `src/op/dynamic_filter`, never the reverse.

```cpp
namespace sirius::op {

/// What is known about a Membership Filter before any batch measured it.
struct static_filter_facts {
  membership_filter_kind kind = membership_filter_kind::none;
  std::uint64_t keys = std::numeric_limits<std::uint64_t>::max();  ///< IN-list key count; unknown for Bloom
  std::optional<double> estimated_keep;  ///< The Producer's static estimate of the kept share
  bool estimate_proven = false;          ///< Whether `estimated_keep` is a Proven Estimate
};

/// The facts of a published filter. Kinds other than a Membership Filter report `none`.
[[nodiscard]] static_filter_facts facts_of(sirius_dynamic_filter const& filter);

/// Filtered Decode's preference among unmeasured filters: less means preferred. Compares kind rank
/// (small IN-list, hash IN-list, Bloom, anything else) and then `keys`; never the estimate.
[[nodiscard]] constexpr std::strong_ordering decode_preference(static_filter_facts const& a,
                                                               static_filter_facts const& b) noexcept;

/// The share of a batch a filter estimated at `own` removes after every filter in `all` estimated
/// strictly stronger. Empty when `own` is empty or nothing in `all` is stronger. `all` may contain `own`.
template <std::ranges::input_range Estimates>
  requires std::convertible_to<std::ranges::range_reference_t<Estimates>, std::optional<double>>
[[nodiscard]] constexpr std::optional<double> removed_share_after_stronger(std::optional<double> own,
                                                                           Estimates&& all) noexcept;

/// The Negligible-Share Rule: whether that share is under the threshold (10%).
template <std::ranges::input_range Estimates> /* same constraint */
[[nodiscard]] constexpr bool negligible_after_stronger(std::optional<double> own, Estimates&& all) noexcept;

/// Whether a Consumer of a Channel holding `published` (in publication order) could select a new
/// unmeasured filter with facts `candidate`: false only when the Negligible-Share Rule skips it AND
/// at least `decode_cap` of `published` are at or before it in `decode_preference`.
[[nodiscard]] bool statically_selected(static_filter_facts const& candidate,
                                       std::span<static_filter_facts const> published,
                                       std::size_t decode_cap) noexcept;
}
```

Consumer changes in `dynamic_filter_merge.cpp` (behaviour-preserving):
- loop :605-623 becomes one call: `negligible_after_stronger(step.estimated_selectivity, program->membership |
  std::views::transform(&selected_step::estimated_selectivity))`;
- `candidate::prior` becomes `static_filter_facts`; `in_decode_order` :492 uses `decode_preference(a.prior, b.prior)`.
  Trap: today's `a.prior == b.prior` compares kind and key count only. Use `decode_preference(...) == 0`, not the struct's
  equality (which would now include the estimate).
- the local constant, struct and function are deleted. No other consumer code changes.

A range-constrained function template is used instead of a policy base class: both callers hold different element types
(`selected_step`, `static_filter_facts`) and there is one rule, not a family.

### 2.2 `construction_plan.{hpp,cpp}` (#2569): the judgement is plan policy

`plan_construction` stays a pure function of plain values. New inputs and outputs:

```cpp
struct binding_facts {
  cudf::data_type probe_type;
  bool accepts_zone_map;
  std::vector<static_filter_facts> published;  ///< NEW: Membership Filters already on the binding's Channel,
                                               ///< in publication order; empty = nothing to judge against
};
struct construction_limits { /* ... */
  std::size_t decode_membership_cap = 1;       ///< NEW: Membership Filters one Filtered Decode carries
};
struct key_plan { /* ... */
  static_filter_facts candidate;               ///< NEW: the planned Membership Filter as Consumers will see it
};

/// Fills each binding's `published` from its Channel's current snapshot (see 3.2 for what counts).
void read_published_filters(dynamic_filter_publish_plan const& plan,
                            std::span<std::vector<binding_facts>> bindings);

/// Whether no Consumer would select the key's planned Membership Filter on any of `bindings`.
[[nodiscard]] bool is_shadowed(key_plan const& key, std::span<binding_facts const> bindings,
                               construction_limits const& limits) noexcept;
```

- `plan_construction` gains step 5b, after `choose_membership` and before the Bloom cap (step 6), so a Shadowed Key
  never pushes its siblings over the cap: fill `candidate`; if `is_shadowed` then `membership = monostate`,
  `fit_pending = false`, reason `SHADOWED`, `++keys_skipped_shadowed`. Nothing else is special-cased: a key left with a
  Zone Map keeps it; a key left with nothing is omitted from `keys` and keeps its line, exactly like `BLOOM_CAP`.
- `candidate.estimated_keep` comes from one function, `admitted_key::estimated_keep(rows)` (moved from the publisher's
  anonymous `estimated_selectivity_of` :200), over the build's keys: `exact_membership::valid_rows` or
  `bloom_membership::rows`. `candidate.estimate_proven` = `domain_gate_eligible()` and a known null count
  (`column.null_rows`) and a known value domain.
- `read_published_filters` is the one adapter from Channels to plain facts. It lives beside `binding_facts_of` so the
  policy of "what counts as published" is unit-testable without a session.

### 2.3 Session (#2570): mechanism only

`dynamic_filter_publication_session` supplies facts and acts on the plan; it holds no rule.
- `begin` (:984): `read_published_filters(plan, bindings)` between `binding_facts_of` and `plan_construction`; passes
  `decode_membership_cap`.
- `state::publish` (:683): stamps `planned.candidate.estimated_keep` / `estimate_proven` on the built filter instead of
  recomputing. What was judged is what Consumers read.
- `state::publish`, before `construction->finish` (:638): re-reads the Channels and ends the attempt when every planned
  key is `is_shadowed` and needs no bounds (section 3.4).

Supporting changes:
- `sirius_dynamic_filter::set_estimated_selectivity(double value, bool proven = false)` and `estimate_proven()`
  (`sirius_dynamic_filter.hpp`, #2569). One setter, so "proven without an estimate" cannot be stored; existing call
  sites compile unchanged.
- `dynamic_filter_publish_plan::decode_membership_cap()`, set by the planner from
  `operator_params.fused_scan.max_membership_sources` next to the other carried limits (#2570). Same per-query value
  the scans use.
- No new setting. The rule has no tunable of its own; it follows the Consumer's constants. (If reviewers insist on a
  kill switch: a SET mapped to "do not call `read_published_filters`"; additive, session-only.)

## 3. Semantics

### 3.1 The predicate

For one Admitted Key with planned Membership Filter `w` (kind, keys, Proven Estimate `e_w`):
- Key Binding `b` is dead iff `!statically_selected(w, b.published, cap)`, that is both
  - negligible: with `S = { p in b.published : p.estimated_keep < e_w }`, `prod(S) < 1` and `prod(S) * (1 - e_w) < 0.10`;
  - decode slots taken: at least `cap` filters in `b.published` have `decode_preference(p, w) <= 0` (ties go to the
    published one: it has the earlier snapshot index).
- The key is a **Shadowed Key** iff `w` has a Proven Estimate, it has at least one binding, and every binding is dead.
- A key without a Proven Estimate is never judged (q9 j29: non-unique `ps_suppkey` / `ps_partkey`).

Checks: q8 j41 0.0067 x (1 - 0.061) = 0.6%, IN-list precedes Bloom, cap 1 -> shadowed. q21 j18 0.04 x 0.513 = 2.1% ->
shadowed. q20 j49 (hash IN-list arriving after a stronger-by-estimate Bloom): the IN-list precedes the Bloom in decode ->
selected -> built. Cap 2: one preceding filter is not enough -> q8 / q21 build, as the Consumer then does probe them.

### 3.2 What counts as published (`read_published_filters`)

Per Channel, from one `snapshot()` (coherent under the Channel lock), each distinct filter object once, in entry order:
- only Membership Filters (`facts_of(...).kind != none`);
- only filters with a Replica on every GPU in the judging session's `plan.replica_spaces()` (`is_available_on_device`):
  on a GPU where the stronger filter is missing, a Consumer would use `w`;
- the estimate of a filter without a Proven Estimate is dropped (`estimated_keep = nullopt`), so it can take a decode slot
  but cannot make `w` negligible;
- nothing at all under a cross-rank merge (`plan.cross_rank_merge()`): the local rows are not the merged filter's keys, and
  ranks must not disagree. The rule is off for distributed plans.

Only published filters count, never planned or announced ones. This is the whole race argument:
- A Channel is append-only and every Replica is ready before a filter is visible, so a counted filter cannot later fail
  or disappear. "The stronger one fails" cannot happen after it was counted.
- A Shadowed Key's filter is never published, so it shadows nobody: no mutual skip, no cycle, at most one loser per pair.
- Keys of the same join never shadow each other (neither is published; either may still be dropped at `finish`).

### 3.3 Safety property (and what it does not cover)

Stated: a key is shadowed only if, on every batch that first meets its Channel's filters after the key would have been
published, on any GPU of the session's Replica set, every Consumer following its static rule skips the filter.
Proof sketch: the planner counts a subset of what the Consumer counts (proven estimates only, each filter once), so its
survival product is >= the Consumer's and its verdict implies the Consumer's; the decode clause uses the Consumer's own
ordering with the Consumer's tie-break. Unit test `consumer agrees with construction` pins it (section 7).

Not covered, because the information does not exist when Construction must decide. All are performance-only:
1. The stronger filter is later measured weak and stopped by the Usefulness Gate; the Consumer would then have used `w`.
   Bounded by requiring Proven Estimates on both sides (exact share of the value domain; wrong only under probe-side
   skew). Price measured on the worst site: q21 forgoes <= 21 ms where the build cost 311 ms (136 ms P2P-equivalent).
2. A batch whose Carrier Type the stronger filter rejects (`accepts_carrier`) while `w` would accept it.
3. Batches already past their first application when `w` would have arrived (re-application skips filters in
   `receipt.applied`). None exist under S6; today 0 of 1700 join-consumer pairs start before publication.
4. Consumers that never run Filtered Decode are judged as if they did (conservative: builds more).

### 3.4 Ordering: two judgement points

- **At `begin`** (creator thread, before any lease): as the prototype. Saves insert, leases, merge, replication.
- **Before `finish`** (Publishing Task, before `construction->finish`): same function, fresh snapshot. If every planned
  key is shadowed and needs no bounds, the attempt ends `SKIPPED` with reason `shadowed_late`; leased storage is released
  by the existing `finalize_input` path. Saves the finish chain (q21 261-267 of 305 ms, q8 32 of 35.6 ms). A partly
  shadowed plan finishes all its keys (dropping one key inside `finish` needs an accumulator API; not worth it for v1).
- Stronger filter published after `w` was published: `w` exists and the Consumer skips it per batch. Same as today.

Why not order-independent: a stronger sibling that has not published can still fail (lease refusal, OOM,
`construction_admission`, inventory mismatch), so skipping on its announced or plan-time estimate could leave the scan
with neither filter, which the Consumer's own rule would not do. Holding `w`'s Construction until its siblings are
Terminal is a scheduling dependency between build pipelines: S6 material, not this change. The second judgement point
turns the 31 ms margin of q21 (stronger IN-list published 31 ms before j18's `begin`) into that plus the whole build
PARTITION, and a flipped order still saves ~85% of the cost and is visible as `shadowed_late`.

### 3.5 Multi-binding, multi-key, transitive, sites

- Multi-binding key: shadowed only if every binding is dead; one live binding builds the filter for all.
- Bindings on a closed Channel are judged like any other (conservative); all-closed stays the existing
  `targets_drained` path.
- Multi-key join: each key judged alone. Partial skip is the ordinary plan shape "some keys have no membership".
- Transitive scan targets and Join-Edge Route Consumers: same rule per Channel; no special case.
- Both Construction Sites (build PARTITION, BUILD_FILTER) share `begin`, so both get the rule.

### 3.6 Terminal (S6 "all Relevant Producers Terminal")

No new terminal state. Every shadow outcome uses an existing completion path:
- all keys shadowed at `begin`: `planned.keys.empty()` -> `end_now(SKIPPED)` -> Producers Terminal at `begin`, earlier than
  today. Under S6 the scan stops waiting for the dead Construction at that moment.
- partial: the session continues; Terminal at publication as today; the shadowed key's line is final from the plan.
- `shadowed_late`: `end_attempt(SKIPPED)` on the Publishing Task -> Terminal before its deposit, as a publication is.
Requirement for the developer: a test asserts `terminal()` on the Channel snapshot in each of the three cases.

## 4. Observability

- Key Outcome reason tokens (`dynamic_filter_key_reason`): `SHADOWED` -> `shadowed` (#2569), `SHADOWED_LATE` ->
  `shadowed_late` (#2570). Line otherwise unchanged: `kind=none pushed=0` unless a Zone Map still publishes.
- Counters (`dynamic_filter_stats.hpp`): `keys_skipped_shadowed` (#2569, in `construction_plan::selection`),
  `keys_skipped_shadowed_late` (#2570). A run where the second is non-zero had a publication-order flip.
- DEBUG line, one per judged key (a key with a planned Membership Filter and a Proven Estimate), emitted by the plan
  code like `[dynamic_filter_domain]`:
  `[dynamic_filter_shadow] at=begin|finish condition=N kind=K keys=N est=X bindings=N dead=N max_removed_share=X decision=skip|build`
- Not in this change: the prototype's Consumer-side `[df_gate]` lines (useful, separate upstream; pre-existing code).

## 5. Docs to update

- `GLOSSARY.md` (#2569): add **Static Selection** (a Consumer's choice among a Channel's Membership Filters before it has
  measured them), **Negligible-Share Rule**, **Proven Estimate** (estimated keep ratio from a build key unique by proof
  or backed declaration, with known null count), **Shadowed Key** (_Avoid_: dead filter, policy-dead, shadow rule for the
  key).
- `docs/super-sirius/dynamic-filters.md` (#2570): "Filter selection" third gate; "Planning" paragraph (step 5b, what
  counts as published); key-line reason list; counters; "Completion and failure" (late abandon); "Limitations" (3.3
  residuals, order dependence, off under cross-rank merge); implementation map row for `static_filter_selection`.
- Header docs: `construction_plan.hpp` ordered step list; `static_filter_selection.hpp` is the cross-module decision's
  single home, and `dynamic_filter_merge.cpp` points to it.

## 6. PR split and commit plan

Rule (pr_split 4): policy, plan, shared definitions and their unit tests -> #2569; session, planner plumbing, call
sites, integration tests, user docs -> #2570. At pr2569 `plan_construction` has no production caller, so everything in
2.2 is dormant there by construction; the only live #2569 change is the behaviour-preserving Consumer refactor.

`prod/shadow-2569` (off pr2569 `a2d6bd5a`):
1. `refactor(dynamic-filter): give the Consumer's static selection one home` - 2.1 + Consumer call sites; new
   `test_static_filter_selection.cpp`. Existing `test_dynamic_filter_merge.cpp` must pass unmodified (that is the
   no-behaviour-change proof).
2. `feat(dynamic-filter): record whether a keep estimate is proven` - setter overload, `estimate_proven()`,
   `admitted_key::estimated_keep`, publisher's anonymous function replaced by the member (same values).
3. `feat(dynamic-filter): plan no membership filter for a shadowed key` - 2.2, reason `SHADOWED`, counter, DEBUG line,
   GLOSSARY terms, plan + key-outcome tests, the `consumer agrees with construction` test.

`prod/shadow-2570`: pr2570's 7 commits **rebased** onto `prod/shadow-2569`, then:
4. `feat(dynamic-filter): skip constructing shadowed keys` - planner cap, `begin` wiring, stamping from the plan;
   session tests. This single commit is the behaviour change.
5. `feat(dynamic-filter): abandon a construction shadowed before it finishes` - 3.4 second point, `SHADOWED_LATE`,
   counter, tests. Separable: reverting it leaves commit 4 intact.
6. `docs(dynamic-filter): document shadowed keys`.

Why rebase, not merge or a single commit on top: CONTRIBUTING prescribes `gh stack rebase` between layers and the queue
squash-merges bottom-up, so #2570 must be a linear series over the final #2569. Expected conflicts when rebasing
pr2570's commits: `construction_plan.{hpp,cpp}` and its test (#2570 adds `bloom_membership::rows`, `bloom_only`,
`fixed_bloom_keys`), `dynamic_filter_stats.hpp`, docs. One semantic point in that resolution: after the rebase the
estimate must read `bloom_membership::rows`, never `sized_rows` (a fixed Bloom key count is not the build's keys).
`dynamic_filter_merge.cpp`, `sirius_dynamic_filter.hpp`, key-outcome files and GLOSSARY are untouched by #2570: no conflict.
Local branches only; `backup.sh` mirrors `prod/*` to the fork under `dfperf/`. No PR, no push elsewhere.

## 7. Test plan

Unit, #2569 (no GPU needed except the Consumer test; filter test doubles suffice for `facts_of` of a Bloom):
- `[static_selection]`: formula table (q8, q21 numbers; exactly 10%; no stronger filter; empty estimates; `own` in
  `all`); `decode_preference` kind rank, key count, equal facts; `statically_selected` for cap 0 / 1 / 2, tie goes to
  published, IN-list after Bloom stays selected; property test over random facts: not selected implies
  `negligible_after_stronger`.
- `[construction_plan]`: skip; no skip without Proven Estimate (candidate or stronger); unknown null count never
  judged; multi-binding with one live binding; partial multi-key; shadowed key keeps its Zone Map; shadowed Bloom key
  does not count toward the Bloom cap; empty `published` = today's plan bit for bit.
- `read_published_filters` on a manual plan: missing Replica excluded; unproven estimate dropped; duplicate filter
  counted once; cross-rank plan yields nothing.
- `consumer agrees with construction` (`test_dynamic_filter_merge.cpp`): Channel with S then W where `is_shadowed` is
  true -> `select_for_decode` does not put W in the first `cap` steps and `prepare_state` does not keep W. And the
  converse fixture (q20 shape) where W is built and the decode takes it.
- `[dynamic_filter_key_outcome]`: tokens `shadowed`, `shadowed_late` (second in #2570).

Session / integration, #2570 (`[publisher]`, `[in_task_publication]`, `[physical_partition]`, `[build_filter]`):
- S published, then W `begin`: W line `shadowed`, no lease taken, Channel Terminal after W's `begin`, counters.
- W `begin`, then S published, then W's final contribution: `shadowed_late`, `finish` not called, storage released at
  `finalize_input`, Terminal. Same with S published after W published: both filters on the Channel.
- Two-key join, one key shadowed: the other publishes; attempt `PUBLISHED`.
- Cap 2; S missing a Replica; cross-rank policy: W built in each.
- Stamp parity: for each kind, `facts_of(*published)` equals the plan's `candidate` (keys for proven keys).
- Multi-GPU variant of the first case under `[dynamic_filter][multi_gpu]`.

TPC-H SF1000 A/B (tip `R/repo` vs `prod/shadow-2570`, protocol of the campaign: fresh process, 1 warm + 5 hot, ABBA,
drop first hot, under the lock; decisive runs first):
1. q21, q8, 3 ABBA blocks each. Expect about -300 ms and -35 ms; `fire`-style check that the skip fires in every
   iteration, cold one included.
2. DEBUG pass of all 22 queries: `[dynamic_filter_key]` lines identical to tip except q8 j41 and q21 j18
   (`kind=none reason=shadowed`); note whether q2 j28 becomes `shadowed_late` (predicted from the 58 ms order, unverified);
   `result.txt` md5 equal (q11 tie order excepted).
3. Controls, 1 block each: q2, q3, q5, q7, q9, q10, q16, q20; then q4, q11, q12, q14, q15, q17, q18, q19, q22 (never
   timed for this change).
4. Arm `SET fused_scan_max_membership_sources = 2` on q8, q21: rule must not fire, key lines equal tip.
5. Quent: 0 consumer scans before publication; q21 j18 / q8 j41 Publishing Task near 0.
Known log difference, not a defect: decode "join filters kept" lines drop (q8 50 -> 16, q21 31 -> 16).

## 8. Verifier issues (VERIFY_PROTO_OP2 section 5) -> resolution

| # | Issue | Resolution | Status |
|---|---|---|---|
| 1 | Order dependence carries the gain | Second judgement before `finish`; `shadowed_late` token + counter make a flip visible | reduced, not removed (3.4) |
| 2 | Mirrors one of two Consumer policies | Predicate includes Filtered Decode order and cap; q20 shape and cap 2 build | solved |
| 3 | One definition; prototype deviations | Shared module; estimate from valid rows, computed once in the plan and stamped; Replica availability checked; proven-only is deliberate and one-sided | solved; carrier is residual 3.3.2 |
| 4 | Wrong estimate of the stronger filter | Proven Estimates on both sides; residual documented with its measured price | bounded, not removed |
| 5 | Reason token, counter, Terminal incl. partial | Section 4, 3.6 | solved |
| 6 | Tests | Section 7 (skip, no-skip, partial, both orders, cross-rank) | specified |
| 7 | Placement | Section 6 | solved |

Also fixed relative to the prototype: decision out of the session; no env switch; own estimate no longer from all rows;
cross-rank disagreement removed by switching the rule off there.

## 9. Out of scope

- q9 j29: the weak key poisons the scan's sticky latch (`pushdown_selection_unprofitable`); Consumer-side, needs distinct
  counts for non-unique keys. Not addressed.
- Prototype variants LATCH (refuted: +11.6 ms), ENDPOINT (+0.4 ms q8), OP3 distinct-aware sizing (no gain): dropped.
- Full order independence (deferring a Construction behind its siblings): belongs with S6 scheduling.
- Dropping a single key inside `finish` for a partly shadowed plan.
- Shadowing under cross-rank merge; Consumer-side diagnostics; any transfer change ([xfer-PR]); a user setting.

## 10. Alternatives considered

| Option | Why not |
|---|---|
| Decision inline in the session (prototype) | Policy inside mechanism; formula duplicated; not testable without a GPU session |
| Judge against planned / announced siblings | A counted sibling can still fail -> neither filter exists; violates 3.3 |
| Defer `begin` or `finish` until siblings are Terminal | New scheduling dependency between build pipelines; S6 |
| Make the filter useful instead (apply after the decode) | Measured: q8 +0.4 ms, q21 -21 ms against a 311 ms build |
| Consumer exports a virtual "would you drop this" interface | The rule is a pure function of three numbers per filter; an interface would leak Consumer state into planning |

## 11. Readings and reference systems (honest account)

- Read for this task: `A Philosophy of Software Design` ch. 4 (deep modules), ch. 5 (information hiding / leakage,
  temporal decomposition) first half, ch. 8 (pull complexity down, configuration parameters), 10.9, 13.7 opening, the
  summary of principles and red flags. `C++ Software Design`: guideline list, summaries of G2, G4, G7, G9, G19, the
  separation-of-concerns part of G2. `C++20 - The Complete Guide`: 5.6 semantic constraints, 5.7 concept design.
  The books total ~2.1 MB; they were **not** read in full (context budget).
- Applied: one home for a cross-module decision (leakage); pure policy vs mechanism; no new configuration parameter;
  special cases folded into the existing "key without membership" shape; constrained range template instead of a base class.
- Reference systems (DataFusion, Velox, DuckDB): **not re-fetched in this task**; from memory only, unverified here: all
  three keep join-derived filters nearly free (bounds, small value sets, or reuse of the join hash table) and rely on
  Consumer-side adaptivity to stop weak filters; none was found to gate construction on filters from sibling joins.
  Sirius differs because its Bloom arrays are dedicated, GiB-sized and replicated per GPU, which is why a
  construction-side gate tied to the Consumer's rule is warranted. A developer or reviewer should confirm before citing.
