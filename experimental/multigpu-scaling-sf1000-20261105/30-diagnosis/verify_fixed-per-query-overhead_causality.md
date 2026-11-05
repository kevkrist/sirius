# Verify (causality): rank 7 "fixed-per-query-overhead"

Verdict: **CONFIRMED** at 0.33 s gap share (claim 0.34). The mechanism needs one correction: "result return" is about 0 ms.
About 91% of the time is planning (DuckDB plus Sirius planning hooks). About 8% is parse plus CLI rendering.

## Checks

1. **Where the Quent window sits (code).**
   - Quent `Init` is emitted when `sirius_engine` is constructed (src/sirius_engine.cpp:120-137).
   - `finished()` is emitted in `~sirius_engine` (src/sirius_engine.cpp:158). The engine is destroyed by the
     `absl::Cleanup` in `sirius_execute_query`, after `engine.get_result()` (src/sirius_interface.cpp:91-110).
   - So result detach/materialization is INSIDE the window.
   - Outside the window is: DuckDB parse, bind and optimize; the Sirius pre-optimizer and optimizer hooks
     (src/transparent/sirius_optimizer_extension.cpp:178-260: `should_use_duckdb`, `add_parquet_footer_statistics`,
     plan `Copy`); finalize-time Sirius plan generation; window begin (physical_sirius_execution.cpp:201-380); and
     DuckDB CLI result rendering.
   - All of this runs strictly before window begin or after window end, on the client thread. Nothing overlaps it,
     so removing it would return its full wall time. No critical-path or link interaction applies.

2. **Phase split from the log timestamps** (`verify_fixed/log_phases.py`, ms resolution, suite sum of hot means).

   | run | timer | plan (QueryBegin→plan generated) | plan→window begin | window | window end→done | unlogged (parse + render) |
   |---|---|---|---|---|---|---|
   | g1_a | 35824 | 395.0 | 2.0 | 35388 | 0.0 | 38.7 |
   | g1_b | 35856 | 397.0 | 1.7 | 35419 | 0.0 | 39.0 |
   | g4_a | 20039 | 402.0 | 1.7 | 19600 | 0.0 | 35.3 |
   | g4_b | 20145 | 395.7 | 0.3 | 19710 | 0.0 | 38.3 |

   - Outside the window: g1 about 436-438 ms, g4 about 434-439 ms. This agrees with Quent (442 / 447).
   - The overhead does not depend on GPU count: per query the g1 and g4 values differ by ≤1 ms (Q21: 35.0 vs 35.7).
   - Post-window time is 0 ms, so "result return" adds nothing.

3. **DuckDB-native vs Sirius-added planning** (CPU-only discriminator, no GPU).
   - Plain DuckDB without the extension ran `EXPLAIN` of the same 22 queries over the same views
     (`verify_fixed/explain_cpu.sql`, `explain_cpu_summary.txt`).
   - DuckDB parse, bind, optimize, physical plan and EXPLAIN render take **234 ms** in total, against Sirius's
     plan phase of **402 ms**.
   - So about 170 ms (about 40%) comes from Sirius planning:
     - the footer-statistics hook. In Q21 nsys there are 211 `open` calls on 93 threads before the window: a
       version check per file on DuckDB workers, with only 18 `pread`, so the cache hits;
     - the logical-plan copy;
     - finalize-time `create_plan`.
   - This share can be addressed inside Sirius without a plan cache. Q21: 18 ms DuckDB vs 35.7 ms in the Sirius plan
     phase. Q18: 12 vs 28.

4. **Inside-window serial overhead** (Quent `first_ms` head plus tail): 24 ms per suite at both g1 and g4.
   Its Amdahl share is about 18 ms. That is too small to change the conclusion, and it is not part of this claim.

## Pricing

- Gap share = out_g4 − out_g1/4.
- Quent gives 447 − 110 = **336 ms**. Logs give g4_a 439 − 109 = 330 ms and g4_b 434 − 109 = 325 ms.
- Corrected estimate: **0.33 s** (range 0.325-0.336), 3.0% of the 11.08 s gap.
- Removing the overhead entirely would save about 0.44 s of g4 wall time, but 0.11 s of that is part of ideal
  linear scaling too.

## Remedy correction

- The remedy should drop "result return".
- Split it into:
  - Sirius-side planning hooks, about 170 ms of suite wall at both g1 and g4: skip the per-file stat/version
    re-check for pinned or unchanged file lists, and avoid the serialized plan copy (low-medium complexity);
  - a plan cache for the DuckDB-native ~230 ms (medium complexity).
