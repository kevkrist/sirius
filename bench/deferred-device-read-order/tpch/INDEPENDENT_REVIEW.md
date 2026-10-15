# Independent review: TPC-H deferred read issuance order

Recommendation: **retain siblings-first; this experiment does not justify switching to own-first or adding a policy knob.** The natural-workload timing differences are small, mixed across queries/regimes, and measured with only two process blocks per policy. The source and counters explain why the controlled benchmark's clean blocked-A benefit does not automatically become a query speedup.

## Verified experiment

Reviewed the amended contract, configurations, original SQL/dataset manifest, SQL runner, generated source transform, diagnostic implementation, orchestrator and analyzer. Completed prescribed skill/measurement-recipe reading and reused actual prior reviewer role/books/engine reading, as recorded in `/tmp/tpch-deferred-order-plan-review.md` and the project `REVIEW.md`. No production edits, GPU jobs or commits by this reviewer.

The current repaired implementation and an order-only alternative were linked into isolated full SQL executables. Independent hash checks matched every authored/production/generated source and all four final binaries to the build manifest. Link maps show the intended single await implementation; neither timing executable links diagnostic objects. Config/query hashes match the run manifest. CPU execution used the same unchanged query SQL and original SF100 parquet dataset; copied CPU outputs retain provenance. The prior 64 MiB calibration was segregated before any primary timing. Primary results use 1 MiB blocks, four reactors, a 64 GiB host pool, and separate default-zero and explicit-eight readahead budgets.

Independent script `/tmp/tpch-deferred-order-independent-check.py` parsed raw TSV/JSON/CSV separately from the parent's analyzer. It verified:

- **224 timing query results and 112 diagnostic query results** against four CPU reference queries, with exact nonfloating values and `abs <= 1e-8 + 1e-10 * max(abs(cpu), abs(gpu))` for floats.
- **32 timing and 16 diagnostic processes**, all exit zero.
- Diagnostic events have zero drops, zero unclaimed-open awaits, no duplicate first-entry cohort IDs, and valid source/stream/device identity for deferred waits.
- Timing medians and balanced per-block deltas were independently recomputed. Initial-context observations are excluded from the fresh/retained figures below.
- All **48 refreshed parent median rows and 32 steady-state paired deltas** match the independent calculations exactly within floating-point serialization tolerance.

Separate attribution validation adds **20 Nsight-run results and 40 Quent-run results**, bringing the independently CPU-checked total to **396**. All **60 GPU-run process manifests** (32 timing, 16 diagnostic, four Nsight and eight Quent) exited zero. The eight Quent families each contain exactly the expected context-initial, fresh and three retained query labels, with Init/Planning/Executing/Exit transitions for all five queries.

Reviewed the final profile summarizer and its output. It correctly nulls Q1/Q19 per-capture query/iteration assignments rather than guessing. Quent operator paths and created/exited task counts match between policies for all four queries; all reported task lifecycles finish. The final `RESULTS.md` interpretation, source citations, measured tables and no-change recommendation agree with this review. No blocking review findings remain.

Independent SQLite inspection confirmed kernels, memcopies and GPU_SCAN NVTX ranges in every available Nsight capture. There are **18 exported captures, not 20**: Q1 and Q19 produced four each rather than five, with roughly doubled activity in the second capture. That suggests combined query windows, but those traces contain no explicit query-iteration labels to prove the exact mapping. Their per-regime capture attribution is therefore ambiguous. Q6 and Q14 each produced five captures. This limits profile attribution, not the separately collected timing or result-correctness evidence. Profiles were not used to price an ordering improvement; summed API/kernel durations are not removable savings.

## Timing interpretation

Each state has six observations per policy/query from two separate process blocks. Positive deltas below mean own-first is slower. Summed query medians are a secondary aggregate, not an actual simultaneous four-query runtime.

| Configuration | Cache state | Siblings-first sum, ms | Own-first sum, ms | Own-first delta |
| --- | --- | ---: | ---: | ---: |
| Default zero readahead | Fresh | 14415.980 | 14439.435 | +0.163% |
| Default zero readahead | Retained | 1811.017 | 1817.204 | +0.342% |
| Explicit readahead 8 | Fresh | 14202.460 | 14199.665 | -0.020% |
| Explicit readahead 8 | Retained | 1785.707 | 1806.834 | +1.183% |

No aggregate materially favors own-first. Fresh per-query paired differences generally change sign or remain small. Explicit-eight retained Q6 and Q19 favored siblings-first in both pairs, but this small experiment does not establish a universal ordering effect or high-confidence tail behavior. Do not describe every difference as proven below noise, or a nonsignificant difference as proof of equivalence; only two independent process blocks per policy were collected.

## How often A was pending

These are **instrumented first-wait observations** for the production siblings-first policy, three fresh iterations per query with explicitly enabled readahead. The relevant ordering denominator is cohorts with another live/open source.

| Query | A pending / all cohorts | A pending / multi-source cohorts | Both A and a sibling pending / multi-source cohorts |
| --- | ---: | ---: | ---: |
| Q1 | 48/54 (88.9%) | 12/15 (80.0%) | 12/15 |
| Q6 | 18/33 (54.5%) | 8/15 (53.3%) | 8/15 |
| Q14 | 20/33 (60.6%) | 9/15 (60.0%) | 9/15 |
| Q19 | 65/66 (98.5%) | 15/15 (100%) | 15/15 |

Across those 60 multi-source cohorts: **44 both pending, zero A-only pending, one sibling-only pending, and 15 neither pending**. Thus every observed pending A with siblings also had a pending sibling. The microbenchmark's blocked-A/ready-siblings case was not observed here. Raw events show these are cross-file cohorts; e.g. Q6 pairs 51 ranges from one lineitem file with 108 from another, or 15 with 144. Slice offsets were not recorded, so calling these exact file tails/heads would exceed the evidence.

With the default zero readahead budget, both policies had **zero pending observations and zero prefetch waits across all 56 diagnostic executions**. With explicitly enabled readahead, all 24 retained-cache diagnostic executions likewise had zero pending observations/waits. These are observed rates in this configuration, not a claim that every installation has readahead disabled.

The actual wait predicate requires a requested range overlapping a loading cache chunk and that handle's producer still in loading state. Readahead has already claimed the fill; demand waits for that producer to finish rather than duplicating its work. A pending entry snapshot is distinct from an actual wait: sibling issuance can consume time during which A completes, and completion can race the observation. Source-side ordering determines whose wait is encountered first when both producers remain active. Pending counts alone do not identify device latency versus reactor queueing, scheduling or other cause, and summed overlapping waits are not removable query time.

## Limits

Selected Q1/Q6/Q14/Q19 at SF100, original local-file layout, one L40S, one host-cache geometry and two readahead settings. This is not all TPC-H, remote storage, another scale, or a broad tuning campaign. Instrumentation scans peers/chunks and can perturb pending races; instrumented elapsed times were never used for the policy comparison. The timing experiment provides a practical no-change recommendation, not proof that ordering never matters. The explicit Nsight window-mapping limitation above remains; no extra measurement was invented to fill it.
