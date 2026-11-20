# Review - shadow rule (prod/shadow-2569 @ 258f8046, prod/shadow-2570 @ 3db07d40)

2026-11-20. Reviewer: adversarial review per `~/AgentDocs/skills/review-harness/SKILL.md`. Single reviewer, no
independent verifier (independence limit). R = `/opt/dlami/nvme/dfperf`.

- Diff bases: `pr2569` a2d6bd5a -> `prod/shadow-2569` (3 commits, 20 files, +1386 -82); `prod/shadow-2569` + pr2570's 7
  rebased commits -> `prod/shadow-2570` (3 commits on top: 42b53087, 45af2166, 3db07d40). Worktrees `R/wt-prod-69`,
  `R/wt-prod-70`, both clean; test binaries newer than the tip commits.
- Contract reviewed: DESIGN_SHADOW_RULE.md (safety property 3.3), IMPL_SHADOW_RULE.md (deviations 1-10).
- Environment: 4x RTX PRO 6000, TPC-H SF1000, config `/home/ubuntu/sirius/specific_config.yaml` unmodified, release build.

**Decision: changes requested (small), then human decision.** No correctness defect and no way to lose a filter the
Consumer's static rule would use was found; the #2569 half stands alone. Requested before merge: F001 (measure or drop
the second judgement point), F002 (adopt a real-Channel randomized agreement test), F003 (log line). F004-F006 optional.
Validation gaps: no sanitizer run, intermediate commits unbuilt, `shadowed_late` unmeasured (see section 6).

## Readings completed (honest)

- Harness: SKILL.md, review-lenses.md, report-format.md, adversarial-validation.md (full). calibration.md not read.
- `A Philosophy of Software Design`: 5.2 information leakage (full), 13.7 cross-module design decisions (opening). Rest not read.
- `C++ Software Design`: DRY passage of Guideline 2 only. Guideline 4 not re-read.
- `CppCoreGuidelines`: F.6 body; F.4, C.86 titles.
- `Optimizing software in C++`: 7.22 virtual functions, 7.23 RTTI. No hot loop is changed; chapters 3/5/10/17 not read.
- `C++20 - The Complete Guide`: located only (views::transform examples); not read.
- CUDA Best Practices Guide: not consulted. N/A: the change has no kernel, stream or device-memory code.
- Campaign: OPPORTUNITIES (USER DECISIONS + sections 1-3 top), durable README, BASELINE_SUITE 0-1.1, tooling 0,
  DESIGN_SHADOW_RULE and IMPL_SHADOW_RULE (full). PROTO_OP2 / VERIFY_PROTO_OP2 not read.
- Code read: full #2569 source diff; #2570 source diff of 42b53087 + 45af2166; Consumer (`dynamic_filter_merge.cpp`
  capture / ordering / `prepare_state` / `select_for_decode`), decode attach path (`sirius_gpu_scan_operator_data.cpp`,
  `compressed_scan.cpp` source ordering, Simpatico cap), planner call site, Channel snapshot. Developer tests: property
  test and Consumer-agreement test read in full; the other new test files by title only.

## 1. Ranked issues

| Rank | ID / title | Priority | Category | Group | Reachability | Evidence | Impact |
|---|---|---|---|---|---|---|---|
| 1 | F001 second judgement point (`shadowed_late`) ships without a measured benefit | P2 | design (scope) | production | default path, never taken in 22 queries | static + developer's logs | unmeasured mechanism on every publication |
| 2 | F002 safety property pinned by one fixture; property test asserts the function's own definition | P2 | test-rigor | tests | - | static + new probe RV-P1 | a Consumer-side change can break the rule unnoticed |
| 3 | F003 `[dynamic_filter_shadow] at=finish decision=skip` printed for keys that are then published | P3 | documentation | production | supported (partly shadowed plan at finish) | static | misleading diagnostics / counts |
| 4 | F004 Consumer-only facts the rule depends on have no pointer to the rule; header says "exactly" | P3 | design | production | - | static | silent drift of a cross-module decision |
| 5 | F005 `binding_facts_of` / `read_published_filters` are coupled by traversal order | P3 | design | production | - | static | two loops must stay in step |
| 6 | F006 an unreadable binding on an empty Channel keeps a key alive | P3 | performance | production | supported, not seen in TPC-H | static | builds a filter no Consumer can use (as at tip) |

## 2. Production findings

### F001 - [P2] The second judgement point is shipped unmeasured
- Location: `dynamic_filter_publisher.cpp` `state::end_if_shadowed` (:619-637) and its call in `state::publish` (:658);
  `construction_plan.cpp` `construction_shadowed`; reason `SHADOWED_LATE`, counter `keys_skipped_shadowed_late` (commit 45af2166).
- Fact: `shadowed_late` occurred in 0 of 22 queries x 2 DEBUG iterations and in no timed run (IMPL section 6); the
  design's predicted site (q2 j28) did not fire. Its claimed saving ("~85% of the cost on a flip") was never measured.
  Unit tests only.
- Cost of carrying it: one more Channel snapshot + re-judgement on every publication, a second reason token, a
  counter, a doc section, and a `finish` path that ends an attempt without publishing.
- Property required: every mechanism in this perf change has a measured benefit on some input (campaign rule: measure
  first). Cheapest ways to establish it: (a) drop 45af2166 from #2570 (the design made it separable; commit 42b53087
  carries the whole measured -343 ms), keep it for the S6 PR where order is the open question; or (b) measure it once
  with a forced flip (delay the stronger join's publication in a test build) on q21 and report the number.
- Rank: highest because it is the only part of the change with no evidence of value; not a defect in behaviour.

### F003 - [P3] `at=finish ... decision=skip` for a key that is published anyway
- Location: `construction_plan.cpp` `construction_shadowed`: logs `judgement.shadowed ? "skip" : "build"` per key before
  the plan-level AND (`shadowed && judgement.shadowed && !key.needs_bounds()`).
- Example: two-key plan, key A shadowed at finish, key B selectable (or A needs bounds). The plan finishes all keys
  (documented), A's filter is published, yet the line reads `decision=skip`. Tools that count `decision=skip` overcount.
- Fix (property: the line states what happened): print the plan's verdict at `finish`, or a distinct token
  (`decision=finish_with_plan`). No TPC-H occurrence (static).

### F004 - [P3] The rule depends on Consumer facts that live outside the shared module, with no back-pointer
- `static_filter_selection.hpp` @file: "judges it exactly as the Consumer will". It mirrors only the unmeasured
  static part. The safety argument also needs, from `dynamic_filter_merge.cpp`: tie-break by snapshot index
  (`in_normal_order`), measured-first decode order (`in_decode_order`), `skip_measured_filter`, `accepts_carrier`,
  `receipt.applied`, and the decode source order in `compressed_scan.cpp`. None of these names `statically_selected`
  (grep: 0 hits in `src/op/scan`). Changing any of them can make the planner skip a filter the Consumer would use.
- Fix: one comment at `in_decode_order` and at the Negligible-Share block naming `statically_selected` as dependent
  (APOSD 13.7), and "the Consumer's static rule" instead of "exactly" in the header. Docs "Limitations" already lists
  the residuals correctly.

### F005 - [P3] Positional coupling of `binding_facts_of` and `read_published_filters`
- `read_published_filters` re-walks `probe_targets()` / `key_bindings` and matches by a per-key counter; identity of the
  plan is checked only by sizes. The two loops must enumerate in the same order forever (APOSD 5.2, same knowledge twice).
- Cheapest fix: let `binding_facts_of` take the "read Channels" choice and fill `published` in its single traversal.
  Optional.

### F006 - [P3] Mixed readability (lead, unmeasured)
- `judge_shadow` treats every binding alike. A key with a readable dead binding plus an unreadable binding whose
  Channel is empty is "live" there and is built, although no Consumer can read it on that binding. Same as tip, so no
  regression; noted by the developer. Scope reduction if it ever matters: judge readable bindings only.

## 3. Test findings

### F002 - [P2] The safety property has a weak oracle
- `test_static_filter_selection.cpp` "a filter that is not statically selected..." (2000 rounds) asserts
  `negligible_after_stronger` and "strictly preferred >= cap": the body of `statically_selected` negated. It cannot
  disagree with the implementation's definition; it says nothing about the Consumer.
- `test_dynamic_filter_merge.cpp` "the Consumer passes over..." is the only Consumer-side oracle: one size pair, one
  column placement, one push order, IN-list only. The developer's own deviation 1 (snapshot order is not publication
  order) was found by accident in such a fixture.
- New probe RV-P1 (section 4) is the missing test: random sizes / estimates / proven flags / caps 0-2 / ties, both
  column placements, both push orders, real Channel + real `dynamic_filter_consumer`; oracle = decode steps and the
  endpoint's output row count. Result: RVP1_RESULT. Recommend adding it (source: `R/runs/review_shadow/probe/rv_probe_merge.cpp`, last test case).

## 4. Validation and new probes

| ID | What | Command / source | Result |
|---|---|---|---|
| RV-P1 | shadowed => real Consumer never uses the candidate (decode slots + endpoint), 600 random trials | `R/runs/review_shadow/probe/{rv_probe_merge.cpp,build_probe.sh,run_probe.sh}`; scratch binary linked from wt-prod-70's objects; `[rv_probe],[scan_merge][shadow]` | RVP1_RESULT |
| RV-U69 | #2569 alone: `[static_selection],[construction_plan],[published_filters],[key_outcome],[shadow],[scan_merge]` on wt-prod-69 | `runs/shadow/bin/unit.sh wt-prod-69 tags ...` | U69_RESULT |
| RV-T21 | independent ABBA, q21, 6 iterations (4 hot kept per process, 8 per arm) | `runs/shadow/bin/abba1.sh rv1 6 21` | T21_RESULT |
| RV-T8 | independent ABBA, q8 | `runs/shadow/bin/abba1.sh rv1 6 8` | T8_RESULT |
| RV-S1 | #2569 uses no #2570 symbol; `plan_construction` / `read_published_filters` have no production caller at 258f8046 | `git grep` on `prod/shadow-2569 -- src` | confirmed (0 callers outside `construction_plan.*`) |
| RV-S2 | Consumer refactor is behaviour-preserving | line-by-line: `in_decode_order` (rank, keys lexicographic = old `static_selectivity` `<=>`), negligible loop incl. NaN and "no stronger" cases, `facts_of` vs `static_selectivity_of` | equal by proof |
| RV-S3 | stamped estimate equals pr2570's | `candidate_of` reads `exact->valid_rows` / `bloom->rows` (:135), as pr2570's `key_rows` | equal by proof |
| RV-S4 | deviation 1 premise | `sirius_dynamic_filter_set::snapshot()` iterates the per-column map; no erase / clear on a Channel | confirmed |

Not run: ASan / UBSan / TSan / LeakSanitizer (need full sanitizer builds; the machine lock was held by 5+ waiters,
flock is unfair); Compute Sanitizer (N/A, no CUDA change); build of the 4 intermediate commits; Quent check of design
test item 5; any q9 re-measure.

## 5. Can a useful filter be skipped? (the question asked)

Checked against the Consumer code, not the design text.
- Later publications: only filters in one Channel snapshot count; planned or announced filters never do. A filter
  published after `begin` is seen only by the second judgement. No mutual skip: a shadowed filter is never published.
- Failed stronger filter: cannot be counted, because it must already be on the Channel with a Replica on every GPU of
  the judging plan (`is_available_on_device` over `plan.replica_spaces()`); Channels are append-only (RV-S4).
- Estimate provenance: `estimate_proven` is set in exactly one production place (`state::publish`, from the plan's
  candidate, never under a cross-rank merge). `publish_dynamic_filters` (still present at #2569) never sets it, so at
  #2569 nothing can shadow. The published side loses its estimate when unproven; the candidate is not judged when unproven.
- Decode slots: scan decode bindings are identity over the scan output or empty (`scan_decode_bindings`), so a
  strictly preferred published filter always precedes the candidate in the decode; ties are not counted (deviation 1 is
  the right, cheapest fix). Join-Edge Route Channels are per key and hold one join's filter: nothing to shadow there.
- Reader AST pushdown is not affected: only Zone Maps are `sirius_ast_lowerable`, and a Shadowed Key keeps its Zone Map.
- Remaining ways (all documented by the design, all performance-only, none removed by this change):
  1. the stronger filter is measured weak (probe-side skew) and dropped by `skip_measured_filter`: the Consumer would
     then have used the skipped filter. The Consumer adapts; the Producer's decision is final. No field switch exists.
  2. carrier type accepted by the candidate but not by the stronger filter.
  3. batches already past first application (`receipt.applied`).
- Not verified: an endpoint whose bindings map the stronger filter's column to `column_index < 0` (would make the
  Consumer ignore it). No such case found in the scan path; not exhaustively traced.

## 6. Design, efficiency, split, docs

- Design: one home for the rule, pure functions over plain values, policy outside the session, no new setting: sound
  (APOSD 5.2; Iglberger DRY). `construction_shadowed` as policy (deviation 3) and the cap as a plain argument
  (deviation 2) are improvements over the design. `static_filter_facts::operator==` has no production user and
  compares the estimate while `decode_preference` ignores it: delete it or keep it test-local (nit).
- Efficiency: no hot loop changed. Per batch the Consumer does the same two `dynamic_cast`s and the same O(n^2) loop
  over a handful of filters as before (Fog 7.22-7.23: cost is per call, not per row). Per join: one Channel snapshot per
  probe target at `begin`, and again at `finish`. No microbenchmark warranted; controls within noise in the developer's table.
- q9: +6.3 ms in the developer's data, same sign in 3 of 3 blocks, inside noise. No mechanism found by reading (key
  lines and stamped estimates equal). Unexplained; worth one more q9 block before the PR text claims "no regression".
- PR split: #2569 = one live, behaviour-preserving Consumer refactor + dormant policy (about 840 lines with tests,
  reason token `shadowed`, counter, GLOSSARY) that only #2570 calls. Consistent with the stack's split rule; the #2569
  description must say the rule is dormant there. It builds and its tests pass alone (RV-U69).
- Docs: `dynamic-filters.md` "Shadowed keys" and "Limitations" match the code, including the tie rule and order
  dependence. Header overclaim: F004. Stats doc "Non-zero means the two joins' publication order flipped" is accurate.
- Human decisions: (1) F001 keep / drop / measure; (2) ship without a kill switch (today only
  `SET fused_scan_max_membership_sources = 2` turns the rule off, and it also changes the decode); (3) accept residual 1
  (skew) as documented.

## 7. Residual risk

- The gain depends on publication order (31 ms margin on q21 today); S6 will change the order question.
- Skewed probe data: the rule removes the Consumer's fallback filter for good (section 5, item 1). Not measured anywhere.
- Unverified: sanitizer cleanliness of the new paths, per-commit buildability, cap-0 mapping at query level, zone-map-on
  path timing, cross-rank stamp.
- Post-merge signals: `keys_skipped_shadowed` (expected 1 per q8 / q21 run), `keys_skipped_shadowed_late` (non-zero =
  order flipped), `reason=shadowed` on any other query = review that site.
