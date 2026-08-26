# PR-4 merge-gate evidence index (design `groupjoin-framework-design.md` section 9, PR-4 row)

All artifacts live under this `pr4/` scratchpad directory unless noted. Baseline = branch
`feat/groupjoin-framework` HEAD 96cf8f2c (PR-1..PR-3 merged); candidate = the PR-4 working tree
built at `build/release/`. Labels throughout: **A = enable_group_join OFF** (yaml copy
`sirius-groupjoin-off.yaml`), **B = enable_group_join ON / default** (`sirius-groupjoin-on.yaml`,
identical to A except the flag) -- both runs on the same final PR-4 binary.

| gate | status | artifact |
|---|---|---|
| count-kernel SASS parity vs ORIGINAL baseline | PASS -- harness self-test PASS (baseline-vs-baseline clean, single-instruction mutation detected); 24/24 baseline count-kernel roles instruction-identical across 8 archs; 81 candidate-only value-bundle roles (whitelisted, unchanged from PR-2/PR-3 -- PR-4 touches no `.cu`). Taken on the final post-clang-format binary | `sass-parity.md` (baseline `../baseline/sass-full.txt`, candidate `sass-final.txt`, harness `../pr2/sass_parity.py`) |
| SF100 kit A/B | PASS -- **q2 FUSES as DIRECT in B**: q2's sirius log carries `Fusing DIRECT MIN into GROUP_JOIN: group key col 0, arg col 1, opaque comparison-join child (est 79996288 rows)` and the runtime strategy line `DIRECT MIN sparse path: state bytes 239997456, input bytes 1585368` (sparse-inside-the-fused-task, exactly section 5.2's own regime; emitted 47481 group rows); A logs no DIRECT fusion. q17-B keeps PR-3's P1 fusion unchanged (`Fusing INNER AVG ... membership publication installed`, `INNER AVG sparse path` filtered regime). Plans: 20/22 queries identical A/B; only q2 and q17 differ, both by their knob-gated fusions (q13's P0 marker identical in both). All 22 `result.txt` byte-identical A/B. q2 best-of-3: 0.1843 s (A) vs 0.1739 s (B), -5.6% -- neutral-or-better, inside q2's historical 0.19-0.58 s swing, no win claimed. q17 best-of-3 0.1776 (A) vs 0.1341 (B). Suite sum-of-best 3.2629 (A) vs 3.0449 (B) | `sf100-plan-parity.md`; run dirs `tpch_20260825_184703_gj_pr4_sf100_A`, `tpch_20260825_185036_gj_pr4_sf100_B`; raw logs `kit-sf100-A.log`, `kit-sf100-B.log` |
| SF1000 kit A/B | PASS with the honest q2 decline -- **q2 does NOT fuse at SF1000**: `GROUP_JOIN DIRECT fusion declined: child estimate 19199950848 bytes (799997952 rows) exceeds the counted-side byte gate 11173625856`. DuckDB's plan-time `EstimateCardinality` for the aggregate's child join returns the full ~800M-row partsupp cardinality (it does not credit the delim-side semi-filter that cuts the true input to ~640K rows / ~10 MB), so 800M x 24 B/row = 19.2 GB > device/24 = 11.17 GB. Reported honestly per the gate definition, not forced; same plan-time-refusal class as q17's documented pre-PR-5 decline, and PR-5's BUILD_STREAM removes this gate. q17-B declines exactly as PR-3 (`counted child estimate 71991962640 bytes ... exceeds ... 11173625856`). Plan parity: 22/22 identical A/B (plan/fusion/strategy/reservation), PASS; all 22 `result.txt` byte-identical; suite sum-of-best 8.7028 s (A) vs 8.6140 s (B), -1.02%, within the +-1.2% noise band (plans identical, so per-query deltas incl. q2 -19%/q22 -21%/q15 +8% are noise, leave-one-out attributes nothing) | `sf1000-plan-parity.md`; run dirs `tpch_20260825_185832_gj_pr4_sf1000_A`, `tpch_20260825_190445_gj_pr4_sf1000_B`; raw logs `kit-sf1000-A.log`, `kit-sf1000-B.log` |
| dense-forcing DIRECT reachability (section 5.2/10) | PASS -- SQL-driven through the FULL planner in the Catch2 integration suite: `gpu_execution group join P2: dense-forcing DIRECT reachability` plans a dense-domain, NULL-key-bearing MIN through rung P2 and asserts the `DIRECT MIN dense path` strategy log (sentinel init + atomicMin + NULL-group slot) plus CPU-oracle-identical results; SUM/MAX/AVG/COUNT_STAR dense variants and the forced-tiny-budget sparse bail asserted in the same case | `test-group_join-final.log` (case 17/81) |
| focused test tags (final binary) | PASS -- `[group_join]` 82 cases / 71088 asserts (incl. the new form/child-count fail-closed wiring test), `[config]` 65 / 710, `[tier_narrowing_policy]` 13 / 111, `[compressed_schema_propagation]` 12 / 238, `[dynamic_filter]` 227 / 2213 (run because the publication target-discovery loop was deduplicated). SASS parity re-verified on the very final relink (`sass-parity.md`, PASS) | `test-group_join-final3.log`, `test-config-final2.log`, `test-*-final.log` |
| pre-commit | PASS on every changed file (clang-format reformatted once mid-stream; every kit/SASS/test gate above was taken on the post-format final binary) | |

Carried polish from PR-3's review, all closed:

- **F2 (discovery-loop duplication)**: extracted `discover_membership_publication_targets`
  (`src/planner/dynamic_filter/dynamic_filter_publication_planning.{hpp,cpp}`); both
  `plan_comparison_join` and `plan_single_key_membership_publication` now call it. The shared
  loop keeps the hash join's per-scan target dedup (the more-tested variant; identical for every
  constructible single-key shape). `[dynamic_filter]` (227 cases, incl. the discovery-parity
  suite) and the group-join publication-parity integration test green.
- **F3 (P1 coverage gaps)**: unit negatives added -- value-aggregate callback spoof (UDF name
  collision, both rungs decline), synthetic ordered value aggregate (SQL cannot build one: the
  binder erases ORDER BY on order-agnostic builtins, so a mutator attaches `order_bys` and the
  screen misses), residual ON predicate (planned as FILTERs above the join; no rung fuses); SQL
  oracle test added for nullable arguments / NULL keys / all-NULL-argument groups through the
  argument-validity gate (`INNER SUM/COUNT_VALID sparse path` asserted).
- **F7 (tautological yaml round-trip)**: `valid_group_join_enable.yaml` now sets the NON-default
  `false` and the test asserts `REQUIRE_FALSE`, proving the key is parsed.
- **F4 (artifact labels)**: A/B labels stated at the top of this index and corrected in both
  generated plan-parity reports (the pr1 script hardcodes a "PR-1 ... pre-refactor" header;
  rewritten to the PR-4 knob-off/knob-on reality).

The full unit suite remains Kevin-gated (machine policy): `pixi run make test`. The SF100/SF1000
kit pairs above provide the end-to-end coverage for this PR's pathway.
