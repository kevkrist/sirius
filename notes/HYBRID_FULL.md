# HYBRID_FULL: one-shot for whole-arriving builds vs PARTITION-only, with the transfer change and eager scans, in ONE binary

2026-11-28. Worktree `$R/wt-hybrid-full`, branch `proto/hybrid-full`. Scripts `$R/runs/hybridfull/bin/`, raw runs
`$R/runs/hybridfull/<tag>_<arm>_{a,b}/`, Quent `$R/telemetry/hybridfull_<label>/q<N>/`, tables `$R/runs/hybridfull/out/`.
(in progress; see checkpoint log at the end)

## Checkpoint log
- 04:47 build ok (interrupted run's uncommitted edits compiled unchanged; its "build failed" was only an interrupted ninja).
  Committed 905b9ade. Unit gate + q5 debug smoke of all arms queued.
- 06:05 unit gate done (expected by-design failures only), smoke ok, m1 chunk 1 done (q3 5 8), chunk 2 running
