# VERIFY_PROTO_EAGER: independent, adversarial verification of notes/PROTO_EAGER.md section 6 (S6 eager scans)

2026-11-27/28. Verifier (not the author). Prototype worktrees untouched (`wt-eager2` fdd2caec + cuCascade patch, built by the
author; tip = `$R/repo` ae939824). Own scripts `$R/runs/veager/bin/`, raw runs `$R/runs/veager/<tag>_<arm><n>/`, Quent
`$R/telemetry/veager_<label>/`, derived tables `$R/runs/veager/out/`. Every GPU process under `flock $R/machine.lock`.
Config untouched, env switches only.

STATUS: IN PROGRESS - see the checkpoint log at the end for what is done and what is queued.

## CHECKPOINT LOG (chronological; newest last)

- 2026-11-27 23:20 UTC. Scripts written: `bin/one_query.sh` (copy of the eager one, output under runs/veager), `bin/unit.sh`
  (arms tip / off / o1 / g1 / f1 / fk1), `bin/plan.sh`, `bin/times.py` (timing table), `bin/mem.py` (peak GPU memory from
  Quent), `bin/cmp_logs.py` (TRACE log comparison tip vs arm), `bin/shapes.py` + `bin/shapes.sh` (non-TPC-H join shapes,
  watchdog, interrupt). Plans: `plans/t1.plan` (ABBA tip / f1 [/ fk1] on q18 q9 q21 q14 q5 q3 q8 + controls q4 q19 q10),
  `plans/lp.plan` (TRACE pass of f1 on the 18 activated queries, g1 on q2 q7 q8 q21), `plans/t2.plan` (binary control:
  `wt-eager2` with eager off vs tip on q9 q18 q14 q3 q21). Progress: `logs/{t1,lp,t2}_plan.out`, `logs/shapes_chain.out`.
- 2026-11-27 23:50 UTC. Recomputed the author's r7 / fs2 sums from raw `runtimes.csv` with my own script: identical
  (-497.2, -596.7, -511.5) -> `out/recalc_r7.md`. Memory table from the author's r7 Quent: `out/mem_r7.md`.
  t1 so far: q18 f1 -87.7, q9 f1 -87.1 / fk1 -116.1 (reproduce). lp: q3 q8 q9 q2 q7 q21 f1 vs tip log pass: same filter keys,
  same rows out of every scan / DYNAMIC_FILTER operator.
- 2026-11-28 00:19 UTC. Interim (tables regenerate with `python3 bin/times.py t1`, `python3 bin/times.py t2 tip off`,
  `python3 bin/shapes_cmp.py tip f1 fk1`): t1 f1 vs tip q3 -46.6, q5 -48.8, q9 -87.1, q14 -52.1, q18 -87.7, q21 -52.5
  (sum -375, author -366 on the same six); fk1 q3 -65.4, q9 -116.1. Binary control t2 (`wt-eager2`, eager off): q9 -9.8,
  q18 +3.4 (noise). Shapes (37 shapes x 2 + 15 interrupts): tip / f1 / fk1 rc=0, results f1 = fk1 = tip on all, 0 HANG,
  13 interrupts each then 16/16 correct follow-up queries. Still queued: t1 q8 q4 q19 q10; lp q14 q15 q22 + g1 pass;
  t2 q14 q3 q21; t3 (g1 vs o1 on q2 q7); shapes s2g1, s2g0, cpu.
