# Verify rank 7 "fixed-per-query-overhead" — measurement lens

Verdict: CONFIRMED. Corrected wall: 0.34 s (range 0.33-0.35) on the 4-GPU suite.

## Re-derived numbers (un-profiled, hot iters 2-4)
Scripts: 30-diagnosis/verify_fixed_overhead/{check.py,edges.py,pre_window.py}; flattened g1_b/g4_b Quent in same dir.

| run | timings total ms | outside Quent window ms | Init->Executing ms | max per-query iter sd (outside) |
|---|---|---|---|---|
| g1_a | 35824 | 442 | 8 | 0.6 |
| g1_b | 35856 | 442 | 8 | 0.7 |
| g4_a | 20039 | 447 | 8 | 0.8 |
| g4_b | 20145 | 441 | 8 | 0.4 |

- Gap share = out_g4 - out_g1/4: a-pair 447-110.5 = 336 ms; b-pair 441-110.5 = 331 ms; mean 333 ms.
- Per query matches claim (Q21 37.3/38.1, Q18 30.4/30.6, Q20 28.6/28.9, Q17 26.7/26.5 ms g1/g4).
- Cross-check: Quent Finished(q_i) -> Init(q_i+1) gaps sum 459-468 ms per pass (outside plus label CALL/CLI print), same at g1 and g4.

## Validity checks
- Profiler artifacts: none. Numbers come from un-profiled runs (CLI `.timer` real minus Quent Init->Finished), not nsys.
  Quent overhead is at most 0.04 s on the whole suite (g4 20.04 with Quent vs 20.00 without), so it cannot inflate this term materially.
- Wall-time, not thread-time: the work runs on the single CLI/client thread, serially, before and after execution.
- Window edges (code): Quent Init is emitted when the engine is constructed (src/sirius_engine.cpp:121-138). That happens after DuckDB parse/bind/optimize and Bind of the prepared statement (src/sirius_interface.cpp:97-102).
  Finished is emitted in the engine destructor (src/sirius_engine.cpp:158), after get_result (sirius_interface.cpp:108).
  So "outside" = parse/bind/optimize + Sirius physical-plan generation in prepare + CLI result print.
- No GPU work outside the window: nsys iter-2 reports with clean pre-windows (g1 Q1,Q6,Q21; g4 Q1,Q18,Q21) show 0 kernels, 0 memcpy and 0-1 CUDA runtime calls (cuProfilerStart) before the `sirius::query` range.
- Cold/warm: Q13 at g1 is unaffected (10.7-11.3 ms in every run). There is no join-policy confound because the term is identical at g1 and g4.
- Double counting: RANKED adjustment 4 sets FIXED to this measured value and removes 0.098 s from the family residuals, so it is not double-booked.

## Corrections / additions
1. Mechanism is more specific than "parse/bind/optimize". A per-query regression gives
   outside_ms ~ 5.5 + 9.4 x (lineitem refs) + 2.4 x (orders refs) + 1.6 x (other table refs), R^2 = 0.875.
   lineitem is 60 parquet files and orders 15, so the cost tracks file count at roughly 0.15 ms per file per reference.
   This is bind-time work over the `read_parquet([file list])` views (multi-file bind / stats, cf. #2447 stats cache), not GPU-related.
   It partly comes from the harness (views over file lists rebound every query).
   Cheaper remedy than a plan cache: cache the bound multi-file/stats per view, or bind pinned tables as catalog tables.
2. There is a small fixed in-window term as well: Init -> first task preparing is 24 ms per suite at both g1 and g4 (planning 8 ms + scan start).
   Its Amdahl share is 24 - 6 = 18 ms. It sits inside the Quent window, so the families or residual already book it, and it is not added to the 0.34.
   The tail (last task finalize -> Finished) is about 1 ms.
3. Share of gap: 0.333/11.08 = 3.0%. The estimate holds; confidence high.
