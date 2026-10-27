# S5 validation

Implementation, source/runner review and local validation are complete.
Four-GPU acceptance awaits the user-run
campaign described in [S5_4GPU_EXPERIMENTS.md](S5_4GPU_EXPERIMENTS.md).

## Baseline and environment

S5 stacks on S4 commit `4d4bfdda12a88ec13b7e577b17ca40feea0316c0`
(PR #2349). cuCascade is `1a9281093a0fe49634594d541d7ce327129f53d6`;
DuckDB is `88ecd0a49d7d3d838e002b2fbc0ce91ccf74110e`.

The local host has two NVIDIA RTX PRO 4500 Blackwell Server Edition GPUs,
32,623 MiB each. Commands use the existing Pixi environment; GPU campaigns run
serially with idle-device checks. The dataset is TPC-H SF100 Parquet, eight
tables and fourteen files. Local raw evidence is under
`/home/ubuntu/sirius/build/s5-validation/`. Artifact names below are relative to
that directory. These results establish two-GPU behavior only.

Production coverage remains 0.9. Timing compares 0.9, explicit 0.45 and 2.0
(coverage gate disabled) using the same S5 binary, with 256 MB scan/concat,
512 MB partition/build, the unchanged 256 MiB Bloom cap and pin compression
disabled. MB batch sizes are decimal; the Bloom cap is 268,435,456 bytes.
Filtering disabled is a separate correctness arm. This comparison does not
measure total S5-versus-S4 overhead.

## Build and focused correctness

| Check | Result | Evidence |
|---|---|---|
| Complete library, unit binary and DuckDB wrapper build | Passed | `build-s5-second.*` |
| Final build after reviewer fixes | Passed, 39.70 seconds | `build-s5-review-fixes.*` |
| Final focused/configuration suite | 71 cases, 8,088 assertions passed | `focused-final.*`, `focused-final-logs/` |
| Native CLI runner integration | Q4 CPU plus four GPU settings passed on the earlier binary | `smoke-q4-cast-pre-review/` |
| All-22 SQL correctness | 88 GPU processes matched 22 CPU oracles; archive audit passed | `all22/`, `all22-command.*`, `all22-audit.*` |
| Changed-file repository hooks | All applicable hooks passed; compiled-source freeze unchanged | `lint-final-first.*` |
| Final portable runner CPU checks | 18 passed, including drift-reporting regression | `portable-runner-agent-checks.json` (developer report) |
| Benchmark key declarations | Six honest single-column keys verified against CPU DuckDB | `tpch-key-audit.*` |

The final suite covers copied source/column/domain evidence, unsupported lineage
and reader options, source mismatch, re-pinning, false declarations including
NULL/duplicate data, CPU/filtering-off result parity, both publisher paths,
zero partial allocation when gated, an ungated sibling, and configuration
validation/SET/RESET. Earlier overlapping test runs are retained but are not
added to the final assertion count.

The reviewer identified two attribution defects: accumulated filters must pass
applicability checks before reporting a coverage skip, and a declaration must
carry pinned provenance before it can enable the gate. Both have minimal fixes
and passing regressions. [REVIEW.md](REVIEW.md) records the independent review.

Orders has 150,000,000 unique keys spanning 1 through 600,000,000. Its domain is
150,000,000 rows, not the width of that range. The other audited declarations are
customer, part, supplier, nation and region keys. No individual lineitem or
partsupp column is declared unique.

The final source/binary freeze is recorded in
`review-fixes-source/manifest.json`, with the exact tracked patch and new
integration test. Library SHA256:
`2a56240809ecfa7188987522b8d10ec83aa370182b9172c405f99ddea03641f0`.
The CLI/extension resolve this worktree's installed library. The focused run and
all-query campaign use this final compiled source; the earlier Q4 smoke does not
establish final-source or performance acceptance.

## Bounded sanitizer evidence

Five declaration/publisher/accumulation cases passed 1,699 assertions under
Compute Sanitizer 2026.3.0.0. With explicit API reporting and API memory-access
checking enabled, memcheck exited 0 with **zero errors**
(`memcheck-explicit-final.*`, `memcheck-explicit.*.log`).

The separate default extended-API run exited 99 with 28 diagnostics. All three
signature groups and counts reproduced in the existing S4 Parquet GPU control:
six HOST/HOST_NUMA pool-access reports, 21 stream-context reports during global
teardown, and one duplicate CUB kernel-registration report. Raw S5/S4 logs and
`sanitizer-comparison.json` remain available. This establishes reproduction
outside S5, not harmlessness or blanket sanitizer cleanliness. No suppression
or disabled API memory-access check was used. The older S4 control is not an
exact-base performance comparison.

## Query and performance campaigns

The final-binary all-22 CPU/DFOFF/0.9/0.45/2.0 campaign passed in 2,641.54
seconds: 88 valid GPU executions against 22 CPU oracles, with settings, GPU
interception and no-fallback checks. The archived raw-evidence audit passed.
The narrowly scoped Q1 `avg_disc` floating-point rule is documented in the
design and runner; all other values and result multiplicities remain exact.

The fixed Q4/Q7/Q10/Q21 screen completed in 1,370.02 seconds with seed 18235:
two fresh-process blocks, two warmups and three measured repetitions per arm.
All 44 processes (20 preflight/diagnostic plus 24 timing) passed, comprising
140 validated GPU executions. The archive audit passed (`screen-audit.*`).

| Query | 0.45 / 0.9 median ratio, block 1 | Block 2 | Screen interpretation |
|---|---:|---:|---|
| Q4 | 1.0138 | 1.0650 | No-gate control; variable timings |
| Q7 | 0.9952 | 0.9992 | No gate difference |
| Q10 | 0.9795 | 0.9867 | No gate difference |
| Q21 | 0.9385 | 0.9167 | Sole qualifying, evidence-active comparison |

Ratios below one favor 0.45. Two blocks do not establish a confidence claim.
Q21's 0.45/2.0 ratios were 0.9596 and 0.9230, so that comparison did not meet
the 5%-in-both-blocks rule. The architect confirmed the bounded continuation:
Q21 plus Q4 at 0.45 versus 0.9, seed 18236, five fresh paired blocks and five
same-arm control pairs per query, each process using two warmups and six measured
executions. The completed confirmation is reported below.

The portable runner's review found that a failed same-arm drift control did not
invalidate its confirmation verdict. The project copy now marks that result
inconclusive while preserving raw ratios/intervals; its 18 CPU checks pass. The
frozen runner used by the completed correctness-only campaign is preserved. No
confirmation result was accepted using the earlier report logic.

Separate DEBUG diagnostics establish the mechanism before interpreting timing:

| Query | Coverage skips at 0.9 / 0.45 / 2.0 | Observed accumulated allocations |
|---|---|---|
| Q21 | 0 / 1 / 0 | 0.45 omits the 146,145,024-byte-per-GPU filter; two smaller filters remain |
| Q10 | 0 / 0 / 0 | Two 11,474,944-byte-per-GPU filters in every arm |
| Q7 | 0 / 0 / 0 | One 23,982,848-byte-per-GPU filter in every arm |
| Q4 | 0 / 0 / 0 | None; suitable no-gate control |

Q21's gated key has 73,072,502 build rows and a copied 150,000,000-row pinned
orders domain (48.715% coverage). The no-hint control retains that count but is
ineligible for the coverage skip. No accumulated cap/resource refusal appeared
in these diagnostic runs. These are separate DEBUG observations, not assumed
per-sample observations for the INFO timing runs. Raw details are in
`screen/` and `screen-evaluation.json`.


## Fresh confirmation and response

The confirmation completed in 1,211.46 seconds: 50 valid processes, comprising
10 preflight/diagnostic executions and 320 warmup/measured executions. Every
result matched its CPU oracle. The raw-evidence audit passed. Evidence:
`confirmation/`, `confirmation-command.*`, `confirmation-audit.*`.

Five fresh paired process blocks were analyzed independently of the screen.
Each process contributes its median over six measured executions after two
warmups; ratios are combined in log space with a block-level Student-t interval.

| Comparison | Geometric ratio | 95% interval | Predeclared disposition |
|---|---:|---|---|
| Q21, 0.45 / 0.9 | 0.94847 | [0.94279, 0.95419] | Gain criterion met; 5.15% lower latency |
| Q4, 0.45 / 0.9 | 1.00564 | [0.99385, 1.01756] | No-gate control passes the 3% non-regression guard |
| Q21, 0.9 / 0.9 | 1.00270 | [0.99675, 1.00869] | Drift guard passes |
| Q4, 0.9 / 0.9 | 1.00942 | [0.98988, 1.02934] | Drift guard passes |

The Q21 latency reduction's interval is 4.58%-5.72%. Both same-arm point ratios
are within [0.97, 1.03] and both intervals include one. This satisfies the
predeclared local confirmation criteria, including the negative control.

The actual INFO timing logs corroborate the changed work. Across all 40 Q21
executions in the 0.45 comparison arm, the large 146,145,024-byte-per-GPU array
was never started; the two smaller filters were started and published on every
execution. At 0.9, all 40 comparison executions and all 80 same-arm-control
executions started and published the large filter as well. No accumulated
cap/resource refusals appeared. Q4 started no accumulated arrays in any arm.
These counts include warmups and measured executions; no samples were excluded
for optional-work differences. See `confirmation-publication-work.json` and the
raw process logs.

Retain the reviewed implementation and the production default of **0.9**. The
result supports a labelled **0.45 experimental profile for Q21 on this two-GPU
host**, not a default change, a broad all-query performance guarantee, or a
measurement of total S5-versus-S4 overhead. It demonstrates that copied pinned
evidence can avoid real filter allocation/construction and improve end-to-end
latency. No further source change is justified by these local results.

The remaining step is the independent four-GPU campaign and returned-evidence
review. Use the fixed protocol in `S5_4GPU_EXPERIMENTS.md`; select any follow-up
from that host's own screen rather than assuming the local Q21 result transfers.
