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
