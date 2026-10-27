# S5: source-bound domain evidence for the existing usefulness gate

Status: implementation contract approved; local confirmation complete as recorded below; four-GPU acceptance pending. This work starts at published S4 `4d4bfdda12a88ec13b7e577b17ca40feea0316c0` (PR2349), including S3c PR2280 and migration PR2302. Keep that dependency baseline; unrelated migration review requests are separate work. Architect owns this file only and ran no builds, tests, or GPU experiments.

## Decision and scope

Extend the existing host-side coverage gate with copied evidence from materialized Parquet pins. A qualifying key may avoid optional filter construction when complete build rows cover the configured fraction of its source domain. Filtering and the authoritative join remain unchanged when evidence is absent. S4 direct/HOST transport, allocation ownership, and the 256 MiB aggregate per-GPU Bloom cap remain unchanged.

Production coverage remains **0.9**. Explicit **0.45** and **2.0** profiles evaluate usefulness; values above one already disable this gate. No new public evidence selector, automatic threshold tuning, distinct-count pass, source-fingerprint framework, or topology work is required.

The domain count describes the unfiltered source, not the filtered scan estimate or `max(key)-min(key)+1`. Exact row count represents distinct-key domain size only with applicable uniqueness evidence. Catalog row bounds remain bounds; caller declarations remain assertions. Neither becomes a correctness proof.

## Pin interface and lifecycle

- Add `PinTableArgs::declared_unique_columns`, exposed in SQL as `unique_cols`, a list of column names. **Each listed column independently asserts uniqueness**; the list does not declare a composite key.
- Nonempty declarations are supported only for materialized `format='parquet'`, `tier='gpu'|'host'` pins. Reject native DuckDB and undecoded `tier='parquet'` declarations clearly before materialization. Empty/omitted declarations retain existing behavior.
- Reject malformed/NULL entries at binding. Resolve names against the source schema and requested pinned columns before materialization/insertion; unknown or unpinned names are errors. Use resolved schema names and deduplicate repeated names. Do not introduce a second schema-binding implementation.
- Add separate `declared_unique_columns` name lists to `pinned_entry_metadata` and `pinned_entry`. They must never populate `proven_unique_columns`, `late_mat::unique_verdict`, `prove_unique_columns`, `unique_build_keys`, distinct joins, or duplicate elimination.
- Publish declarations through existing atomic insertion metadata, alongside the entry. No post-insert setter or separate registry mutation. Failure leaves the prior published generation intact; existing conservative epoch-bump behavior need not change.
- **Replacement:** retain only declarations supplied by the new pin. This includes HOST/device-chunk replacement and source-changing replacement. Unpin/reset removes the entry and its hints.
- **Same-source/same-row-count GPU merge:** preserve existing declarations and add validated incoming declarations for retained or newly stored columns. Omission/empty declarations do **not** revoke an existing hint on this merge path. Revocation uses unpin followed by pin without the hint. Keep existing merge layout/placement validation.

Restricting new hints to materialized Parquet is deliberate: a native pin caches a checkpointed prefix while later queries can include MVCC deletes and insert deltas. A pin-time assertion is not current-table evidence; extending it would require separate snapshot/staleness handling. Native scans retain their existing catalog-bound and genuine-uniqueness behavior. An undecoded Parquet cache does not supply the materialized entry used here.

## Planner and runtime contract

1. Evolve `build_key_domain*` to collect one plain evidence value per **original condition**: `{cardinality, source, declared_unique}`. Source values are `unknown`, `catalog_bound`, `pinned_parquet_exact`. Its constrained callable accepts `(LogicalGet const&, scan_output_ordinal)` and returns that value. Keep this a small value interface; no inheritance hierarchy or persistent cache.
2. Reuse `resolve_scan_column_origin(..., origin_policy::row_subset)`. Preserve the source output ordinal through projections/filter maps, then map through `projection_ids` and `GetColumnIds()` to the actual source column. Reject unsupported computed/cast keys, row-multiplying joins, multiple grouping sets, unresolved CTE lineage, and virtual/nested/generated columns whose identity is not established.
3. New pinned evidence requires a trusted plain-Parquet connector, the **complete canonical file set**, and matching source column ID/name/type in a materialized pin. Reordering files is acceptable; subsets, supersets, repeated-file inputs, and different sources do not borrow evidence. Do not infer Parquet eligibility merely because the shared resolver can return Iceberg files.
4. Reader semantics must also match the pin. Use existing verified source information where sufficient; otherwise conservatively admit known equivalent/default Parquet reader options. Schema overrides, transformations, or unproven mappings/options disable optional evidence. Paths alone do not prove equivalence. Do not add a fingerprint-capture framework.
5. Use existing owning registry lookups under the existing stable planning window. Validate and copy counts, provenance, and flags before `create_plan` moves logical children. No pin/scan pointers enter the execution plan; no new registry read lock spans GPU work. Existing pin epochs govern transparent-plan rebuild and fragment invalidation after registry changes.
6. Pass the evidence vector to `admit_dynamic_filter_keys`, keeping its optional genuine-uniqueness argument separate. Preserve `build_key_domain_cardinality` and `build_key_proven_unique`; add `build_key_domain_source` and `build_key_declared_unique` to admitted metadata. Reject misaligned evidence vectors as programmer errors. Unknown source evidence yields no hint eligibility.
7. Only the shared usefulness decision combines genuine proof and caller-hint eligibility. Gate use requires an applicable key, positive domain/build rows, `build_rows <= domain`, and an enabled valid threshold. Unknown/zero/inconsistent evidence disables **this gate**, not filtering. Known duplicated or row-multiplying lineage cannot establish eligibility. A false caller assertion is not independently verified: it may suppress a useful optional filter, but must never change SQL results.
8. Apply the same policy in ordinary publication and `try_begin_accumulation`; accumulation already evaluates selection before geometry and partial allocation. Preserve sibling-key admission, terminal producer completion, errors, cancellation, and S4 retirement rules. A gated key contributes no partial arrays.
9. Centralize the unchanged `0.9` default in `src/op/dynamic_filter/config.hpp`, referenced by publication policy and operator defaults. Preserve finite-positive validation, values above one as off, YAML defaults, per-session SET behavior, and RESET to the registered configured default.

Catalog provenance is explicitly different from pinned provenance: retain the current native `NodeStatistics::max_cardinality` contract and genuine uniqueness checks. Do not relabel it as an exact pinned distinct count or use a native cached-prefix count as the full domain.

## Diagnostics

Reuse existing counters (`keys_considered`, `keys_with_known_domain`, `keys_build_exceeded_domain`, `keys_skipped_domain_gate`, accumulation starts/completion and refusal counters). No new SQL statistics interface or benchmark-only production counter family.

Provide one stable host DEBUG record per bound key considered, for both paths:

```text
[dynamic_filter_domain] path=ordinary|accumulated condition=N rows=N domain=N provenance=unknown|catalog_bound|pinned_parquet_exact eligible=0|1 threshold=X decision=skip|keep
```

An optional trailing `reason=` token is allowed. `eligible` means applicable source/uniqueness and consistent positive counts; threshold-off can leave eligibility true but decision `keep`. `skip` means the coverage gate **actually applied**, not a prospective predicate superseded by a type mismatch. Accumulated logging must respect its applicability checks. Condition numbers are local to each join, not globally unique. A tiny private logging helper is sufficient.

Run DEBUG diagnostics separately from INFO timing with the same SQL, data, hints, settings, and binary. Count both paths and validate logged skips against existing counters in integration tests. Diagnostic decisions demonstrate mechanism for that run; they are not invented per-sample measurements for separate timing runs. If decisions vary, retain the distribution and qualify attribution.

## File ownership

| Owner | Files and responsibility |
| --- | --- |
| Kant: production | `src/pin_table.hpp`, `src/sirius_extension.cpp`, `src/scan_manager/sirius_scan_manager.{hpp,cpp}`: argument validation and atomic hint metadata |
| Kant: planner | `src/planner/dynamic_filter/build_key_domain.{hpp,cpp}`, `dynamic_filter_key_admission.{hpp,cpp}`, `src/planner/sirius_plan_comparison_join.cpp`: source/column evidence and admission |
| Kant: publication/config | `src/op/dynamic_filter/dynamic_filter_publish_plan.hpp`, `dynamic_filter_source_policy.hpp`, `dynamic_filter_publisher.cpp`, `config.hpp`, `src/sirius_config.hpp`: copied values, policy, diagnostics, centralized default |
| Kant: owning docs | Relevant paragraphs in `docs/super-sirius/scan.md`, `dynamic-filters.md`, and `configuration.md`; explain hints, supported scope, defaults and reset behavior without unrelated cleanup |
| Helmholtz: tests | Relevant `test/cpp/{planner,scan_manager,operator,integration,config}` files and new focused integration tests. Preserve Kant's completed guard tests. |
| Poincare: harness | `/home/ubuntu/sirius/build/s5-validation/s5_compare.py` only; reusable local/SSH-host experiment mechanics and analysis |
| Parent | Test-source/CMake registration, builds, GPU scheduling, remote access, evidence manifests, `S5_VALIDATION.md`, integration and final review coordination |
| Architect | This `S5_DESIGN.md` only; subsequent design assessment against implementation/results |

## Required validation matrix

| Layer | Cases and required outcome |
| --- | --- |
| Pure policy | Domain 100, rows 0/44/45/89/90/100/101 at thresholds 0.45/0.9/2.0; exact threshold fires, zero/unknown/over-domain does not; unknown uniqueness does not gate. Keep invalid-threshold config tests. |
| Lineage/admission | Projection and filter remapping, reordered original conditions, independent keys, supported row subsets; casts/computed keys, multiplying joins, multiple grouping sets and unresolved lineage lose evidence. Composite uniqueness does not prove either component. |
| Source identity | Complete multi-file source and permuted file order; subset/superset/duplicate files, same basename/different source, column mismatch, reader-option mismatch, absent pin and unsupported cache tier. Only exact applicable evidence is copied. |
| Pin validation/lifecycle | Independent names, duplicate names, NULL/unknown/unpinned names, explicit native/undecoded rejection; merge preserve/add/omission, replacement clear, unpin/reset, failed insertion, epoch-invalidated prepared-plan reuse. No stale metadata reference. |
| Proof separation | False declarations over duplicate/null-bearing data preserve exact multiplicity and CPU parity, including aggregation/joins where a false correctness proof would fail. Measured late-mat proofs and distinct-join choices remain unaffected. |
| Publisher/allocation | Ordinary and accumulated paths agree; gated key allocates no partials, ungated sibling still publishes; inconsistent rows retain filtering; terminal rights resolve without publication. Existing S4 failure/ownership coverage remains applicable. |
| Sparse domains | Equal row counts for dense keys and unique keys with large holes produce the same coverage decision; range width is never substituted. Include sparse `o_orderkey` in SQL diagnostics. |
| Configuration | Default 0.9, explicit 0.45/2.0, YAML override, SET/RESET and session isolation; cap/transport unchanged. |
| SQL | CPU oracle, GPU filtering off, and gate profiles agree. Require transparent GPU interception with fallback disabled for GPU evidence; retain failures/skips explicitly. All 22 TPC-H queries receive correctness checks before timing conclusions. |

Parent runs focused native tests and an appropriate bounded sanitizer check of changed integration paths; this metadata change does not require replaying every dependency-only transport experiment. Record actual commands/counts and sanitizer mode in `S5_VALIDATION.md`. No tests were run by the architect.

## Experiment protocol, fixed before measurements

**Environment and contrast.** Local screening uses two RTX PRO 4500 GPUs with 32 GB each and SF100 at `/home/ubuntu/sirius-s3c-data/tpch_parquet_sf100`. Parent's `build/s5-validation/data-preflight.json` records footer counts, including orders 150 million rows/two files and lineitem 600,037,902 rows/six files. Capture actual GPU UUIDs/topology/driver, source/library/CLI hashes, complete file manifest, settings, SQL, runner hash, seed and logs for every campaign.

Use the **same S5 binary and memory/batch configuration** for all primary arms. Start from the parent's valid local S4 capacity profile: 256 MB scan/concat (256,000,000 bytes) and 512 MB partition/build (512,000,000 bytes), coverage as below, unchanged S4 transport, and the separate 256 MiB Bloom cap (268,435,456 bytes). Save the complete effective configuration rather than relying on these headline values. Hold compression, consumer gate, residency, budgets, and other optimizations constant. Remote memory sizing may differ by host but cannot differ between arms on that host.

| Arm | Settings and purpose |
| --- | --- |
| P09 | Honest hints, filtering enabled, coverage 0.9: production regime |
| P045 | Same, coverage 0.45: explicit experimental regime |
| GOFF | Same, coverage 2.0: only coverage gate disabled |
| DFOFF | `enable_dynamic_filter=false`: GPU correctness/control result; not interchangeable with GOFF |
| NOHINT | Omit declarations at 0.9: diagnostic ablation confirming new pinned evidence matters |
| CPU | Matching native DuckDB CLI, GPU execution disabled: correctness oracle |

Declare only honest single-column keys when they are among the columns pinned: customer `c_custkey`, orders `o_orderkey`, supplier `s_suppkey`, part `p_partkey`, nation `n_nationkey`, region `r_regionkey`. Never declare lineitem or partsupp composite-key components independently unique. Freeze per-query pin lists across arms; NOHINT changes declarations only.

The older `/build/s4-perf/final` CLI at revision `1cfab` is not the exact S4 base; it may supply explicitly labelled supplemental baseline evidence only after parent verifies the known diagnostic/doc-only difference. It is unnecessary for the primary same-binary threshold contrast. Do not claim that GOFF measures total S5-versus-S4 overhead.

1. **Deterministic micro checks:** policy matrix above plus real SQL over fixed dense and sparse unique domains, build fractions 0.10/0.50/0.95, and duplicate/false-hint negatives. Run profiles against CPU/DFOFF, verify actual gate decisions and no-partial-allocation behavior. Small fixture runs demonstrate mechanism, not production speedup; no new standalone publication schedule.
2. **Correctness and diagnostic preflight:** all 22 available TPC-H queries, one execution in P09/P045/GOFF/DFOFF plus CPU oracle, with exact configuration/interception proof. Parent reports the native Q4 CPU/DFOFF/three-arm smoke passed and 18 runner CPU tests cover multiple-repetition framing. **Accepted before timing data:** omit the redundant four-query pilot. After all-22 correctness passes, proceed directly to the fixed screen below. Its first process validates native warmup/multiple-repetition framing; any framing or result-validation failure aborts the campaign with raw failure evidence preserved. NOHINT at 0.9 is a separate diagnostic control for these queries. Confirm actual pinned provenance and gate-firing differences before claiming gate-related timing effects. If diagnostics show no actual gate firing, retain them and report an unexercised usefulness experiment without timing claims; this is not by itself an engine-correctness failure. Comparisons, screen block counts, and confirmation rules are unchanged.
3. **Local screen:** Q4/Q7/Q10/Q21, three threshold arms only, **two fresh process blocks, two warmups and three measured repetitions per arm/query/block**. Randomize query and arm order with saved seed; run serially without competing GPU work/builds. Q4 is a candidate negative control, only treated as such if diagnostics show no relevant gate difference. No confidence claim from two blocks.
4. **Continuation rule:** select at most one strongest evidence-active query/profile contrast showing at least **5% improvement in both screen blocks**, with consistent direction and no confirmed correctness/problem signal. Keep its comparator and one observed no-gate control. Confirm rather than expanding the screen matrix. A suspected regression may receive one similarly bounded diagnosis/confirmation instead of winner confirmation.
5. **Fresh confirmation:** **five fresh paired process blocks, two warmups and six measured repetitions per arm/block**, for the selected contrast and control; add **five same-arm B/B process pairs** for each selected query to assess drift. Analyze each pair's measured median ratio, then equally weight its log ratio across blocks; use a 95% Student-t interval over the five independent blocks. Repetitions within a process are not independent clusters.
6. **Four GPUs:** user will run the campaign after parent completes local experiments/review and publishes the authorized fork-branch snapshot while preserving the local unstaged diff. The draft `S5_4GPU_EXPERIMENTS.md`, portable runner/selfchecks, and `s5_pinned.yaml` under `tools/experiments/` now exist. The handoff's Q4 correctness smoke checks the new host and does not restore the removed timing pilot. Repeat all-22 correctness and the three threshold-arm diagnostic/screen comparison on the actual four-GPU host; apply the same bounded confirmation rule. Two-GPU results cannot satisfy four-GPU acceptance. If no four-GPU contrast merits confirmation, report that outcome rather than inventing a winner. Four-GPU acceptance remains pending returned results.

**Predeclared decisions:** a measured useful gain requires geometric ratio at most **0.95** and upper 95% bound below **1.00**. The no-regression criterion is upper 95% bound at most **1.03** for each declared confirmation/control comparison; call a regression confirmed when the lower bound exceeds **1.03**. Results crossing 1.03 are inconclusive, not passes. B/B drift must have point ratio within **[0.97, 1.03]** and interval including one; otherwise attribution is inconclusive. Report wide intervals and timing resolution honestly. These focused guards do not prove a broad all-22 performance guarantee.

All valid timing samples stay in the intended feature contrast, even when optional publication or `filters_pushed` differs. Keep those observations; do not silently require equal work, discard policy skips, or treat a cap/resource refusal as gate success. Exclude invalid execution/parity/profile samples only by frozen rules, retaining raw evidence and invalidating incomplete pairs. Do not use minimum warm time or resume incompatible manifests.

The runner must use matching native CLI/library, fail-fast SQL, process timeout, strict output framing, effective-settings proof, GPU interception/fallback checks, and selected-GPU idle checks. Preserve raw timing precision and typed results. Do not copy the old harness's generic numeric tolerance: integers/decimals/NULL/types/shape remain exact. If all-22 needs the already diagnosed Q1 exception, restrict it to recognized Q1 `avg_disc` DOUBLE column 8 with unique exact row keys, absolute OR relative tolerance `1e-12`; literal digests may differ. No approximate multiset matcher or global tolerance expansion.

Results drive bounded changes: fix correctness/source-contract defects first; trace an inert gate to evidence/admission before adding timings; diagnose a measured regression at the smallest affected seam. A 0.45-only gain supports a labelled experimental profile, never an automatic default change. Preserve 0.9 regardless of one-host results. No compelling gain means report the scope/limits and assess the small evidence extension on its demonstrated value; it does not authorize cap removal or advanced topology work. Parent records actual evidence and reviewer acceptance separately.

## Reference comparison and reading record

Personally reused prior substantive readings: Ousterhout, *A Philosophy of Software Design*, chapters 2 (complexity), 4 (deep modules), 5 (information hiding/leakage), 8 (pull complexity downwards), and chapter 20 sections 20.1–20.5 (performance); *C++ Software Design*, chapter 1 Guidelines 1–5 and chapter 3 Guidelines 11–14; the supplied *C++20 — The Complete Guide* draft, chapter 5 (Concepts and Requirements) and chapter 7 (Ranges and Views). These are section/chapter claims, not full-book claims. No new book reading was needed for this extension.

Refreshed architect role, Codebase Design and Document Code skills; reused the previously read complete Document Code syntax reference. Refreshed worktree AGENTS and Super Sirius README reading order, architecture overview/query lifecycle, memory ownership, join planning, pinned scan identity/merge/MVCC, dynamic-filter evidence/publication/gating, and late-mat uniqueness sections. Loaded `.claude/skills/module-context/SKILL.md` and actual DuckDB planner/catalog/storage module docs; current source overrides stale generated member spellings.

Inspected local pinned DuckDB `88ecd0a49d7d3d838e002b2fbc0ce91ccf74110e`: `NodeStatistics`, native `TableScanCardinality`, Parquet `GetCardinality`, join-filter pushdown, and `JoinFilterPushdownInfo::CanUseBloomFilter`. Lesson: separate statistical usefulness from correctness eligibility; Parquet's callback estimates are not a guaranteed domain. Native max-cardinality retains its existing bound contract. Prior narrow Velox HashJoinBridge and DataFusion hash-join/shared-bounds readings supply only completeness/ownership context; no GPU performance claims follow from CPU references.

Inspected actual integrated reference `ef17c8c55003dbc889e93c24eedf55f127f22c7a` with read-only `git show`: domain provider, per-column declarations, pin metadata, admission and gate/config wiring. Reuse the idea of exact pinned rows and per-column usefulness assertions. Do not transplant raw pin-pointer memoization, name-only native matching, post-insert declaration mutation, misleading folding into `build_key_proven_unique`, the public evidence selector, or its 0.45 default. That historical stack is not reproduced by this same-binary threshold experiment.

Issue1824's supplied acceptance contract and local rollout section guide this plan; live retrieval failed in this architect session, so no fresh issue-body verification is claimed. Parent independently refreshed it. Implementation, runtime outcomes, the user-run four-GPU handoff and measured performance remain separate evidence to be recorded after this design pass.

## Implementation and local-validation checkpoint

Retain the reviewed implementation and proceed to the authorized four-GPU handoff. The small metadata extension uses existing insertion, lineage, publication and lifetime mechanisms; its demonstrated value does not require new topology, configuration or measurement machinery. Parent reports source approval, 71 focused cases/8,088 assertions passed, all-22 parity (88 GPU executions plus 22 CPU oracles), and archive/source checks passed. Sanitizer evidence is bounded to explicit API reporting with zero errors; the extended-API diagnostics reproduced in the S4 Parquet control are documented in `S5_VALIDATION.md`, not a blanket default-mode clean claim.

The architect read the completed two-GPU SF100 confirmation summary, manifest/completion, publication-work summary and audit. The campaign contains 50 GPU processes/330 executions, including diagnostics and warmups; timing inference uses only the five fresh process-pair blocks per comparison, with the prescribed B/B guards.

| Comparison | Geometric time ratio, 95% interval | Disposition |
| --- | --- | --- |
| Q21, 0.45 / 0.9 | 0.948473 [0.942788, 0.954193] | Meets predeclared gain criterion: 5.15% point improvement, interval 4.58–5.72% |
| Q4 negative control, 0.45 / 0.9 | 1.005638 [0.993852, 1.017563] | Passes 3% no-regression guard |
| Q21 B/B | 1.002704 [0.996755, 1.008689] | Drift guard passes |
| Q4 B/B | 1.009421 [0.989883, 1.029344] | Drift guard passes |

DEBUG evidence identifies Q21's applicable pinned domain: 73,072,502 build rows / 150,000,000 source rows. Coverage 0.45 skips the large filter; 0.9 and 2.0 retain it. INFO confirmation logs show zero versus 40 starts of the 146,145,024-byte-per-GPU Bloom across the 40 executions of each comparison arm, including warmups; both smaller filters persist and neither arm reports refusals. This supports the intended work-avoidance mechanism and an end-to-end local benefit; it does not isolate individual kernel/transport costs.

Keep production **0.9**. Label **0.45** only as a supported experimental profile on this measured local workload; these results establish neither a broad all-22 performance guarantee nor S5-versus-S4 overhead or four-GPU acceptance. No additional local experiments or code changes are justified by this result. Parent's final lint and authorized snapshot publication can proceed while preserving the local diff; the user-run four-GPU campaign and returned-evidence review remain the next acceptance step.

Local provenance: `/home/ubuntu/sirius/build/s5-validation/confirmation/{summary,experiment,completion}.json`, `confirmation-publication-work.json`, and `confirmation-audit.json`/`.log`; parent owns the detailed validation record and published evidence packaging.
