# IMPL_SHADOW_RULE: production implementation of the shadow rule

2026-11-19. Developer write-up for DESIGN_SHADOW_RULE.md. STATUS: see section 0 (rewritten as results arrive).
R = `/opt/dlami/nvme/dfperf`. Nothing pushed except by `backup.sh` (fork, `dfperf/prod/*`). No PR.

## 0. Status

RESULTS_PLACEHOLDER

## 1. Readings completed (honest)

- `C++ Software Design`: guideline list; Guideline 2 (design for change: SRP, DRY, premature separation) in full; Guideline 4
  (testability, free functions) second half + summary; summaries of Guidelines 7, 8, 22. Not read in full (939 KB).
- `C++20 - The Complete Guide`: 3.2.2-3.2.4 (comparison categories, `operator<=>`), 5.6 (semantic constraints), 5.7 (concept
  design, concepts vs traits). Not read in full (844 KB).
- `CppCoreGuidelines`: rule titles of F, I, C (1-12, 20, 45-49, 131, 160-163), Con, ES (selected), Enum, T (1-3, 10-13, 20-21,
  40-49). Rule bodies not re-read.
- `Optimizing software in C++` (Agner Fog): NOT read. The change is plan-time policy over a handful of values per join; no hot
  CPU loop was written, and no architecture-specific efficiency claim is made.
- Campaign: OPPORTUNITIES (USER DECISIONS + sections 1-3 top), durable README, BASELINE_SUITE section 0-1.1, tooling 0, 1, 7,
  DESIGN_SHADOW_RULE (full), GLOSSARY (full), dynamic-filters.md (sections touched). PROTO_OP2 / VERIFY_PROTO_OP2 / pr_split /
  construction were NOT re-read (the design quotes what was needed); the prototype diff of the publisher was read.
- Code read at pr2569 / pr2570: `dynamic_filter_merge.cpp` (selection parts), `construction_plan.{hpp,cpp}`,
  `dynamic_filter_publish_plan.{hpp,cpp}`, `sirius_dynamic_filter.hpp` (base class, Channel), `dynamic_filter_key_outcome`,
  `dynamic_filter_stats.hpp`, the session's `begin` / `state::publish` / attempt helpers, the test fixtures used.

## 2. What was built

One rule, one definition, two callers.

- `src/op/dynamic_filter/static_filter_selection.{hpp,cpp}` (new): Static Selection. `static_filter_facts` (kind, key count,
  estimate, proven flag), `facts_of(filter)`, `decode_preference` (kind rank, then key count), `removed_share_after_stronger` /
  `negligible_after_stronger` (constexpr range templates constrained by the concept `keep_estimate_range`, 10% threshold
  `negligible_removed_share`), `statically_selected(candidate, published, decode_cap)`.
- `src/op/scan/dynamic_filter_merge.cpp`: the Consumer calls those functions; its local constant, struct, function and loop
  are deleted. Behaviour-preserving (existing `[scan_merge]` tests unmodified until the last #2569 commit adds one case).
- `sirius_dynamic_filter::set_estimated_selectivity(value, proven = false)` + `estimate_proven()`;
  `admitted_key::estimated_keep(build_keys)` replaces the publisher's anonymous `estimated_selectivity_of`.
- `construction_plan.{hpp,cpp}`: `binding_facts::published`, `construction_limits::decode_membership_cap` (default 1),
  `key_plan::candidate`, `read_published_filters(plan, bindings)` (the only function that reads Channels),
  `is_shadowed(key, bindings, decode_membership_cap)`, step 6 of `plan_construction` (before the Bloom cap, now step 7),
  DEBUG line `[dynamic_filter_shadow]`, and in #2570 `construction_shadowed(planned, bindings, decode_membership_cap)`.
- Observability: reasons `SHADOWED` -> `shadowed`, `SHADOWED_LATE` -> `shadowed_late`; counters `keys_skipped_shadowed`,
  `keys_skipped_shadowed_late`; `to_string(membership_filter_kind)` moved next to the enum (was private to key_outcome.cpp).
- Session (#2570): `begin` calls `read_published_filters` and passes the cap; `state::publish` stamps the plan's candidate
  estimate (+ proven flag, never proven under a cross-rank merge) and calls `end_if_shadowed()` before
  `construction->finish`. Planner: `dynamic_filter_publication_policy::decode_membership_cap` =
  `enable_fused_scan_filter ? fused_scan.max_membership_sources : 0`.
- Docs: GLOSSARY (Static Selection, Negligible-Share Rule, Proven Estimate, Shadowed Key); dynamic-filters.md (new
  "Shadowed keys" section, Planning, Completion and failure, reason list, Limitations, implementation map, test tags).

## 3. Branches and commits

`prod/shadow-2569` (worktree `R/wt-prod-69`, off pr2569 `a2d6bd5a`; 20 files, +1386 -82):

| commit | subject | files / lines |
|---|---|---|
| b68201ad | refactor(dynamic-filter): give the Consumer's static selection one home | 6 files, +499 -50 |
| 2bee8412 | feat(dynamic-filter): record whether a keep estimate is proven | 6 files, +50 -15 |
| 258f8046 | feat(dynamic-filter): plan no membership filter for a shadowed key | 12 files, +838 -18 |

`prod/shadow-2570` (worktree `R/wt-prod-70`): pr2570's 7 commits rebased onto `prod/shadow-2569` (2ed31cf9 .. 85ec1584), then:

| commit | subject | files / lines |
|---|---|---|
| 42b53087 | feat(dynamic-filter): skip constructing shadowed keys | 5 files, +412 -11 |
| 45af2166 | feat(dynamic-filter): abandon a construction shadowed before it finishes | 8 files, +161 -10 |
| 3db07d40 | docs(dynamic-filter): document shadowed keys | 1 file, +27 -6 |

`git diff pr2570 prod/shadow-2570`: 23 files, +1982 -105. No co-author trailers. Author identity as the stack's.
#2569 uses no #2570 symbol: it builds and its tests pass on their own (section 5). `plan_construction` has no production
caller at pr2569, so the only run-time change there is the Consumer refactor.

Rebase conflicts met (resolved in the rebased pr2570 commits, recorded by rerere in `R/repo`):
- `construction_plan.hpp`: `construction_limits` members (kept all three), the @file paragraph (both).
- `dynamic_filter_stats.hpp`: counter list and the doc rewritten by pr2570's style commit (counter doc re-added in the new
  per-counter list form).
- `dynamic_filter_publisher.cpp`: `estimated_selectivity_of` is deleted by #2569 and still used by pr2570's new
  `state::publish` -> call replaced by `admitted_key::estimated_keep` in the first rebased commit.
- Semantic point from the design: `candidate_of` reads `bloom_membership::rows` after the rebase (`sized_rows` at pr2569,
  where the two are the same number). Done in the first rebased commit (2ed31cf9).

## 4. Deviations from the design, with reasons

1. **A tie in decode preference does not count for the published filter** (design 3.1 said ties go to the published one,
   "it has the earlier snapshot index"). `sirius_dynamic_filter_set::snapshot()` walks an `unordered_map` of columns, so
   snapshot order follows the Channel's columns in hash order, not publication order: on a tie the Consumer may put the new
   filter first, the decode then carries it, and the design's safety property would not hold. `statically_selected` now counts
   only strictly preferred published filters. Consequence: a Bloom filter is never shadowed by another Bloom filter (equal kind,
   both key counts unknown). q8 j41 and q21 j18 are Bloom-behind-IN-list and are unaffected. Found by a failing unit test.
   `binding_facts::published` is documented as unordered for the same reason.
2. `is_shadowed` takes the cap (`std::size_t`), not the whole `construction_limits`: it reads nothing else, and the session's
   second judgement has no limits object to pass.
3. The second judgement is a plan-policy function, `construction_shadowed`, not a loop in the session (design 2.3 put "every
   planned key is `is_shadowed` and needs no bounds" in `state::publish`). The session keeps no rule.
4. `shadowed_late` is recorded directly (lines set to `SHADOWED_LATE`, `keys_skipped_shadowed_late` = planned keys, attempt
   ends SKIPPED) rather than through the `attempt_counter` mechanism, which would have printed
   `attempt_skip_<counter>` and counted 1 per attempt.
5. Planner cap is 0 when `enable_fused_scan_filter` is off (design: copy `max_membership_sources`): without Filtered Decode
   every filter goes through the Negligible-Share Rule, which is exactly cap 0. Unit-tested at cap 0; not exercised by a query.
6. The published filter's estimate is never stamped proven under a cross-rank merge (not in the design): the merged filter
   holds other processes' keys, so the local share is not its share.
7. `removed_share_after_stronger` / `negligible_after_stronger` are not `noexcept` (design signatures were): they are
   templates over arbitrary input ranges, whose iteration may throw. `statically_selected` and `is_shadowed` are `noexcept`.
8. `read_published_filters` tests live in a new file `test_dynamic_filter_published_filters.cpp` (needs a GPU memory space:
   an enabled plan requires a replica space), and the session tests in a new file
   `test_dynamic_filter_shadowed_construction.cpp`, instead of being spread over `[publisher]` / `[in_task_publication]` /
   `[physical_partition]` / `[build_filter]`.
9. A shadowed key's line follows `BLOOM_CAP` exactly, as the design asks: if its zone map publishes, the line's reason becomes
   `-` (kind=none zone_map=1). Zone maps are off by default, so in the benchmark the line reads `reason=shadowed`.
10. GLOSSARY: "shadow rule" is an Avoid word only "for the key"; the docs name the judgement by its outcome (Shadowed Key).

## 5. Test evidence

Scripts: `R/runs/shadow/bin/{build,unit,one_query,abba1,dbgn}.sh`, `abba_table.py`, `keylines.py`; all GPU work and builds
under `flock R/machine.lock`. Unit status log: `R/runs/shadow/unit/status.txt`; outputs `R/runs/shadow/unit/<worktree>/`.

UNIT_PLACEHOLDER

New test cases (all tagged `[dynamic_filter]`):
- `test_static_filter_selection.cpp` `[static_selection]` (8 cases, host only): formula table (q8 / q21 numbers, the exact 10%
  boundary, no stronger filter, own estimate in the range, product of several), `decode_preference`, `facts_of` on stand-ins,
  proven flag, `admitted_key::estimated_keep`, `statically_selected` at cap 0 / 1 / 2, IN-list after Bloom, ties, and a
  2000-round random property (not selected => negligible and >= cap strictly preferred).
- `test_dynamic_filter_construction_plan.cpp` `[construction_plan][shadow]` (4 new cases): skip, cap 2, weak published filter,
  unproven published estimate, IN-list behind Bloom, candidate facts, uniqueness by proof or backed declaration, unknown value
  domain, unknown null count, valid-row estimate, multi-binding, `is_shadowed`, partial multi-key, zone map kept, Bloom cap.
- `test_dynamic_filter_published_filters.cpp` `[construction_plan][published_filters]` (4 cases): zone map ignored, unproven
  estimate dropped, one filter on two columns counted once, re-read sees later publications, per-binding Channels, bindings
  of another plan refused, cross-rank merge counts nothing, missing Replica excluded.
- `test_dynamic_filter_merge.cpp` `[scan_merge][shadow]` (1 case): the Consumer and `is_shadowed` agree on a real Channel
  (shadowed: decode prefers the published filter and the endpoint skips the other; converse: decode takes the candidate).
- `test_dynamic_filter_key_outcome.cpp`: tokens `shadowed`, `shadowed_late`.
- `test_dynamic_filter_shadowed_construction.cpp` `[construction][shadow]` (7 cases, #2570; two real sessions on one Channel):
  all keys shadowed at `begin` (no lease, Channel Terminal, counters, lines, storage returned); partial two-key join; built at
  cap 2 / unproven published estimate / unproven keys / empty Channel; stamp parity for Bloom, hash IN-list, small IN-list;
  two GPUs (`[multi_gpu]`: shadowed with a Replica on both, built with a Replica on one); late abandon (`shadowed_late`,
  nothing merged, storage returned, Terminal); stronger filter after publication (both stay); partly shadowed late plan
  finishes all keys.

## 6. TPC-H SF1000 results

BENCH_PLACEHOLDER

## 7. Known gaps / unverified

GAPS_PLACEHOLDER
