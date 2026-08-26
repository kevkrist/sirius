# PR-1 merge-gate evidence index (design `groupjoin-framework-design.md:981`)

All artifacts live under this `pr1/` scratchpad directory unless noted. Baseline = branch HEAD
45228fc9 (pre-refactor, binaries archived in `../baseline/`); candidate = the PR-1 working
tree built at `build/release/` (extension mtime 2026-08-25 00:54, verified newer than every
changed source file).

| gate | status | artifact |
|---|---|---|
| SASS parity on count kernels | PASS -- 24/24 kernel roles instruction-identical across 8 archs; non-vacuous (baseline dump has 192 `dense_count_join_impl` / 0 `group_join_impl` markers, candidate dump the inverse) | `sass-parity.md` (inputs `../baseline/sass-full.txt`, `sass-new.txt`; generator `sass_parity.py`) |
| allocation-set parity | PASS -- q13 SF1 trace-level reservation logs A/B: all 9 per-pipeline reservation requests byte-identical (fused pipeline 2: 295,560,832 bytes), plan render / fusion decision / dense-strategy lines / 43-row CSV results identical modulo rename; log-A raw contains `DENSE_COUNT_JOIN`, log-B zero occurrences | `allocset-parity.md` (generator `allocset_compare.py`, inputs `allocset/`) |
| SF1000 kit A/B neutral within run noise | PASS -- best-of-3 suite total 8.632 s (A) vs 8.676 s (B), +0.51%; per-query deltas within noise (largest +10.8% on the 90 ms q11) | `plan-parity.md` runtime table; raw logs `kit-A.log` (`../kit-A.log`), `kit-B.log`; run dirs `test/tpch_performance/output/tpch_20260825_001121_gj_pr1_A` (baseline binary -- its logs show `DENSE_COUNT_JOIN`) and `tpch_20260825_014000_gj_pr1_B` (candidate) |
| plan-parity check on all 22 queries | PASS -- per-query Sirius plan render (Pipeline Overview + DAG), fusion-decision, runtime-strategy, and reservation lines identical A/B modulo the mechanical rename, for q1-q22; q13 non-vacuous (fusion 1, strategy 2, reservation 1) | `plan-parity.md` (generator `plan_parity.py`) |
| full unit suite | KEVIN-GATED -- not run (machine policy). Focused subsets green against the shipped binary: `[group_join]` 37 cases / 826 asserts, `[config]` 64, `[tier_narrowing_policy]` 12, `[compressed_schema_propagation]` 11 (`test-*.log`). Command for Kevin: `pixi run make test` or `timeout 45m ./build/release/extension/sirius/test/cpp/sirius_unittest --abort` (CI-equivalent); expect the known ~5/2692 machine-specific timeout flakes | `test-*.log` |
| full SQLLogic suite | WAIVED (documented) -- `test/sql/*.test` is legacy-`gpu_processing`-only and this build has `ENABLE_LEGACY_SIRIUS=OFF` (pre-existing; CI does not run these files either); Super-Sirius SQL coverage stands in via the Catch2 integration suite + kit A/B result parity | `sqllogic-waiver.md`, failure log `test-sqllogic.log` |

No code was changed while assembling this evidence: `git status` still shows exactly the 24
PR-1 rename/modify entries, and the kit output directory is gitignored.

Review round 2026-08-25: the strict reviewer's sole remaining finding (major, process) restates
the KEVIN-GATED row above -- confirmed accurate against `groupjoin-framework-design.md:981`, no
code change required or made; working tree, built extension (mtime 2026-08-25 00:54, still newer
than every changed source), and all focused-test logs unchanged and green since the evidence was
assembled. Action stays with Kevin: run the full-unit-suite command in the table row above and
archive the output here as `test-full.log`.

Review round 3 (2026-08-25): the same finding was re-filed and re-verified -- the design-doc PR-1
gate row still requires the full unit suite, `test-full.log` is still absent here, and the
focused-subset numbers cited by the reviewer match `test-*.log` exactly. Machine policy still
reserves full-suite runs for Kevin, so no code, build, or test artifact changed this round
(`git status` unchanged at the 24 PR-1 entries; extension mtime 2026-08-25 00:54 verified newer
than every source under `src/` and `test/cpp/`). This row is the single open merge gate.
