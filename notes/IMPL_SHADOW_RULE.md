# IMPL_SHADOW_RULE: production implementation of the shadow rule

2026-11-19. Developer write-up for DESIGN_SHADOW_RULE.md. STATUS: see section 0 (rewritten as results arrive).
R = `/opt/dlami/nvme/dfperf`. Nothing pushed except by `backup.sh` (fork, `dfperf/prod/*`). No PR.

## 0. Status

DONE (2026-11-20 01:30). Both branches built, DF unit tests green on both, 22 queries checked against the tip, rule fires
exactly at q8 j41 and q21 j18, suite -342.8 ms (-2.24%) in ABBA. Open points in section 7.

| | |
|---|---|
| `prod/shadow-2569` | `258f8046` (3 commits on pr2569 `a2d6bd5a`), worktree `R/wt-prod-69`, built |
| `prod/shadow-2570` | `3db07d40` (pr2570's 7 commits rebased + 3), worktree `R/wt-prod-70`, built |
| Suite (22 queries, ABBA, median of 8 hot per arm) | tip 15319 ms -> 14976 ms, **-342.8 ms (-2.24%)** |
| q21 / q8 (2 ABBA blocks, 16 hot per arm) | **-306.3 ms (-21.3%)** / **-35.1 ms (-3.75%)** |
| Results | 21 of 22 byte-identical; q11 identical as a row set (tie order differs between any two runs) |

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

| branch @ commit | command (under the lock, in the worktree) | result |
|---|---|---|
| prod/shadow-2569 @ 258f8046 | `CUDA_VISIBLE_DEVICES=0 pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]"` | All tests passed (137554 assertions in 636 test cases) |
| prod/shadow-2569 @ 258f8046 | `pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter][multi_gpu]"` (4 GPUs) | All tests passed (4650 assertions in 32 test cases) |
| prod/shadow-2570 @ 3db07d40 | same single-GPU filter | All tests passed (142738 assertions in 660 test cases) |
| prod/shadow-2570 @ 3db07d40 | same multi-GPU filter | All tests passed (4652 assertions in 32 test cases) |
| prod/shadow-2570 @ 3db07d40 | `"[static_selection],[construction_plan],[published_filters],[key_outcome],[shadow]"` | All tests passed (2437 assertions in 51 test cases) |

Each ran once. `pixi run pre-commit run --files <changed files>` passes on both branch tips (clang-format, codespell, cmake,
rumdl, orphaned-test check). `pixi run make` (release) succeeded on both worktrees. Only the two branch tips were built and
tested; the intermediate commits were not built one by one.

History of the test runs (kept honest): the first run of the new tests failed 2 cases / 6 assertions, both test errors
(a wrong arithmetic expectation; an assumption that a Channel snapshot is in publication order). The second led to
deviation 1 and to rewriting the session tests around a real second join instead of a Bloom stand-in.

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

Protocol: fresh process per query, 6 iterations (1 warm + 5 hot, first hot dropped -> 4 hot per process), ABBA = tip A,
prod A, prod B, tip B in one lock hold (`abba1.sh`), so 8 hot per arm and block; medians. Tip = `R/repo` (ae939824), prod =
`R/wt-prod-70` (3db07d40). Config `/home/ubuntu/sirius/specific_config.yaml` unmodified. 104 processes, all rc=0. Block a1 =
all 22 queries; block a2 = q9 (x2), q21, q8. Raw: `R/runs/shadow/{a1,a2}_{tipA,prodA,prodB,tipB}`, table
`R/runs/shadow/abba_table.md`, telemetry `R/telemetry/shadow_*`.

| q | tip med ms (n) [min-max] | prod med ms (n) [min-max] | delta ms | delta % | per-block deltas ms | results |
|---|---|---|---|---|---|---|
| 1 | 472.3 (8) [461-481] | 467.7 (8) [462-485] | -4.6 | -0.97 | -4.6 | identical |
| 2 | 149.6 (8) [148-152] | 149.9 (8) [147-153] | +0.4 | +0.23 | +0.4 | identical |
| 3 | 661.9 (8) [656-666] | 661.7 (8) [657-748] | -0.2 | -0.02 | -0.2 | identical |
| 4 | 324.7 (8) [323-327] | 324.9 (8) [323-329] | +0.2 | +0.07 | +0.2 | identical |
| 5 | 690.8 (8) [678-717] | 691.9 (8) [678-713] | +1.1 | +0.16 | +1.1 | identical |
| 6 | 380.7 (8) [379-384] | 380.6 (8) [380-383] | -0.1 | -0.02 | -0.1 | identical |
| 7 | 747.0 (8) [741-753] | 745.6 (8) [739-753] | -1.4 | -0.19 | -1.4 | identical |
| 8 | 936.6 (16) [929-939] | 901.5 (16) [891-983] | -35.1 | -3.75 | -35.0 -35.9 | identical |
| 9 | 1414.3 (24) [1390-1443] | 1420.6 (24) [1398-1440] | +6.3 | +0.45 | +10.1 +11.6 +0.8 | identical |
| 10 | 861.4 (8) [850-870] | 861.0 (8) [842-872] | -0.4 | -0.05 | -0.4 | identical |
| 11 | 175.1 (8) [171-181] | 173.3 (8) [169-180] | -1.8 | -1.02 | -1.8 | tie order differs in every run, tip vs tip included |
| 12 | 702.4 (8) [700-704] | 703.2 (8) [700-704] | +0.8 | +0.12 | +0.8 | identical |
| 13 | 1304.6 (8) [1267-1386] | 1301.2 (8) [1262-1391] | -3.5 | -0.27 | -3.5 | identical |
| 14 | 688.3 (8) [677-707] | 692.6 (8) [676-706] | +4.3 | +0.63 | +4.3 | identical |
| 15 | 528.5 (8) [526-530] | 526.7 (8) [525-532] | -1.7 | -0.33 | -1.7 | identical |
| 16 | 308.5 (8) [303-316] | 314.5 (8) [302-318] | +6.0 | +1.95 | +6.0 | identical |
| 17 | 497.5 (8) [487-508] | 497.2 (8) [487-508] | -0.3 | -0.06 | -0.3 | identical |
| 18 | 1096.2 (8) [1078-1124] | 1092.1 (8) [1079-1099] | -4.1 | -0.38 | -4.1 | identical |
| 19 | 879.8 (8) [877-892] | 880.7 (8) [878-888] | +0.9 | +0.10 | +0.9 | identical |
| 20 | 717.9 (8) [697-736] | 715.2 (8) [700-734] | -2.7 | -0.38 | -2.7 | identical |
| 21 | 1441.6 (16) [1430-1463] | 1135.3 (16) [1121-1165] | -306.3 | -21.25 | -302.3 -311.0 | identical |
| 22 | 339.1 (8) [338-340] | 338.5 (8) [338-340] | -0.6 | -0.17 | -0.6 | identical |
| sum | 15319 | 14976 | -342.8 | -2.24 | | |

- Gain is where predicted and nowhere else: q21 -306.3 ms (blocks -302.3, -311.0), q8 -35.1 ms (-35.0, -35.9); design
  expected about -300 and -35. All other queries within +-6.3 ms and +-2%, inside the campaign noise (+-3% / +-10 ms).
- q9 is +6.3 ms (+0.45%) over 3 blocks (+10.1, +11.6, +0.8); ranges overlap fully (tip 1390-1443, prod 1398-1440). Not
  distinguishable from noise with this data; q9 has no shadowed key and no proven key on j29. q16 +6.0 ms is one block.
- Suite: 15319 -> 14976 ms. The tip total here is 53 ms below the campaign reference (15372 ms): different day and median
  of 8 instead of 6; compare within this table only. Against the reference base (15225 ms) prod is ~250 ms faster; that
  comparison mixes runs and is indicative only.

Log pass (DEBUG, 2 iterations per query = cold + hot, all 22 queries on both binaries, `dbgn.sh` labels `dbg_tip`,
`dbg_prod`; comparison `keylines.py dbg_tip dbg_prod`, saved in `R/runs/shadow/keylines_all.txt`):
- `[dynamic_filter_key]` lines are identical between tip and prod for 20 queries. They differ only at q8 join 41 and q21
  join 18, in both iterations: `kind=bloom size=182278944 pushed=1 reason=-` -> `kind=none size=0 pushed=0 reason=shadowed`
  (q8, 91139462 rows) and `kind=bloom size=1461613440` -> `reason=shadowed` (q21, 730806711 rows).
- `[dynamic_filter_shadow] ... decision=skip` appears 2 times in q8 and 2 times in q21 (once per iteration, the cold one
  included) and in no other query (148 `[dynamic_filter_shadow]` lines in total over the 22 queries, `begin` and `finish`). q8: `at=begin kind=bloom keys=91139462
  est=0.0608 bindings=1 dead=1 max_removed_share=0.00626`; q21: `est=0.4872 ... max_removed_share=0.0205`.
- `shadowed_late` never appears. The design's guess that q2 j28 would become `shadowed_late` did not happen: q2's lines
  equal the tip's.
- Results (`result.txt` md5, hot iteration): 21 of 22 equal. q11 differs in md5 but is the same multiset of rows (sorted
  files identical, 936989 rows); its tie order also differs between tip A and tip B.
- 0 lines matching `execution fallback|fallback to DuckDB|falling back|[error]` on either binary.
- Arm `SET fused_scan_max_membership_sources = 2` (q21, q8; labels `dbg_tip2`, `dbg_prod2`): the rule does not fire
  (`dead=0 decision=build`), key lines and results equal the tip's.

Not measured: Quent check of the design's test plan item 5 (consumer scans before publication; Publishing Task time of
q21 j18 / q8 j41). The telemetry of every run is kept under `R/telemetry/shadow_*` for it.

## 7. Known gaps / unverified

- Intermediate commits were not built or tested individually (2 of 3 on #2569, 2 of 3 on #2570); only the tips were.
- `shadowed_late` (second judgement) is covered by unit tests only; no TPC-H query exercises it, so its saving (design:
  ~85% of the cost when the order flips) is unmeasured.
- The gain depends on publication order (design 3.4). It held in every observed iteration, cold ones included: both
  DEBUG iterations per query, and in each of the 4 timed prod processes per query the INFO line `construction started`
  appears exactly once per iteration less than on the tip (q21 18 vs 24, q8 24 vs 30 over 6 iterations). Nothing
  guarantees the order; under S6 the question changes.
- Bloom-behind-Bloom is never shadowed (deviation 1). If such sites matter, `read_published_filters` would have to carry
  each published filter's Channel column and the Consumer's tie order would have to become deterministic first.
- A binding that cannot read the key's membership filter still counts as a binding on which a Consumer could select it
  (conservative: builds more). Not special-cased, as in the design.
- Residuals of design 3.3 stand as documented in dynamic-filters.md (stronger filter later measured weak, carrier
  mismatch, batches already past first application, non-decode Consumers, a stronger filter not bound to a decoder slot).
- A shadowed key with zone maps enabled still runs Construction for its bounds; untimed (zone maps are off by default).
- Cap 0 planner mapping (`enable_fused_scan_filter = false`) and the cross-rank "never proven" stamp have no query-level run.
- Reviewer question carried over from the design: #2569 touches pre-existing Consumer code (`dynamic_filter_merge.cpp`,
  -50 lines) and `sirius_dynamic_filter.hpp`; the shared definition requires it.
- Reference systems were not consulted by the developer either.
- Quent verification not done (section 6). nsys not run.
- Lock contention: `flock` is not fair; some of these runs waited 25+ minutes. Timings were always taken under the lock.
