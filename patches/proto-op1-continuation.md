# proto/op1-continuation (worktree wt-op1a) - recovered

OP1 design A: asynchronous publication continuation with a join at the build deposit. Throwaway prototype.
Result (see notes/PROTO_OP1A.md, notes/VERIFY_PROTO_OP1A.md): works, no lost filter, gain = HOST-bounce only
(-167 ms / -1.09% suite, 4 GPUs); zero on the PEER pair. Recommendation was: drop for this campaign.

Recovery status: EXACT. Every source edit was replayed from the transcripts (python in-place edit heredocs, in order)
onto pr2570 `ae939824`. Checks: (1) `git diff ae939824 HEAD -- <7 src files>` is byte-identical to the same command's
output captured by the verify agent; (2) `git diff --stat` identical (9 files, +1216 / -7; bench file 914 lines);
(3) bench file lines 60-432 after the cherry-pick identical to the transcript's `sed -n` output; (4) the cherry-picked
commit's diff is identical to the independently recovered `mb-lifecycle.patch`. Not compiled after recovery.

Files: `proto-op1-continuation.patch` = `git diff pr2570 HEAD`; `proto-op1-continuation.commits.mbox` = the 6 commits
(`git am`). Restored worktree: `/opt/dlami/nvme/dfperf/wt-op1a` (local commits, never pushed; submodules NOT initialized).

## Commits (original hash -> recovered hash; messages prefixed "recovered prototype: ")

| original | recovered | content |
|---|---|---|
| 43f9d882 | c51a2c6f | async continuation + join point: dynamic_filter_publisher.{cpp,hpp}, hash_join_partition_observer.cpp, sirius_physical_hash_join.{cpp,hpp} |
| b8cb3c4e | b1cb5128 | cherry-pick of mb/lifecycle a5b37728: bench_dynamic_filter_lifecycle.cpp (MB0/MB2-MB5/MB7), cmake/sirius-test-sources.cmake, complete_build_inventory.cpp |
| 78665a9c | be069f01 | MB8 microbench (sync vs async publication, with cross-GPU copy load) |
| 3e4dfb51 | ac2117f8 | early-launch variant toggle |
| 1914a8df | c7915e0e | MB8: keep the load's target stream alive until its buffer is freed |
| acc10949 | 61eb1453 | SIRIUS_PROTO_NUM_GPUS override in src/sirius_config.cpp |

Notes refer to the ORIGINAL hashes.

## Switches (env vars)

- `SIRIUS_DF_ASYNC_PUBLISH=0` : prototype binary publishes synchronously (default = async on). Arm `S*`.
- `SIRIUS_DF_ASYNC_EARLY=1` : launch the continuation before the hash path's scatter (off by default). Arm `E*`.
- `SIRIUS_PROTO_NUM_GPUS=<n>` : measurement-only override of `topology.num_gpus` (use with `CUDA_VISIBLE_DEVICES=0,1`).
- `SIRIUS_PROTO_BLOOM_CHUNK_MIB=<n>` : from mb/lifecycle, Bloom transfer chunk cap (MB4).
- Bench: `SIRIUS_BENCH_DF_GPUS`, `SIRIUS_BENCH_DF_REPS` (MB8, Catch2 tag `[mb8]`).
- Log lines tagged `[df_async]` (field `start_delay_us` etc.) feed `runs/op1a/bin/site.py` / `gain.py`.

## Build / run (all under `flock $R/machine.lock`, R=/opt/dlami/nvme/dfperf)

- First: `git -C $R/wt-op1a submodule update --init --recursive` (not done by recovery).
- Build: `flock $R/machine.lock $R/runs/op1a/bin/build.sh` (pixi run make; log runs/op1a/logs/build.log);
  unit-test binary only: `build_ut.sh`.
- A/B: `flock $R/machine.lock $R/runs/op1a/bin/pair.sh <label> <q> <iters> A1 B1 B2 A2`
  (A* = tip $R/repo, B* = proto, E* = early, S* = proto sync); per-query ABBA queue: `abba4.sh <label> <iters> <q...>`;
  chained holds: `chain.sh`. Tables: `table.py [--arms=A,B] <label...>`, per-site: `site.py`, `gain.py` (`ARM_A=S`), `timeline.py`.
- Microbench / unit tests: `mb.sh <label> <gpus> <catch filter> [VAR=VALUE...]`; `aux1.sh`, `aux2.sh`, `aux3.sh` = the
  exact holds used.
- Scripts depend on `$R/runs/profile/bin/qload.py` (other group) and `$R/runs/baseline/bin/*`.

## Scripts and notes (all EXACT)

- `scripts/runs/op1a/bin/` (16 files): heredocs + in-place edits replayed; 11 cross-checked against later `cat` output.
- `scripts/runs/vop1a/bin/` (19 files): verify agent's Write calls; `vsite.py` / `vgain.py` regenerated with the
  original `sed` from op1a `site.py` / `gain.py`.
- `notes/PROTO_OP1A.md`: replayed (Write + append + 4 edits); the data-derived timing table was taken from the verify
  agent's full Read of the final file; result is byte-identical to that Read.
- `notes/VERIFY_PROTO_OP1A.md`: Write + 1 edit replayed.

## Not recovered / unfinished

- Raw data: runs/op1a/{ab1,ab2,dbg,early,g2,g2smoke,mb,smoke,sync4}, gain_ab1.md, table_ab.md (table is inside the
  note), runs/vop1a/{v1,g2,g2p23,trace,ut,edge,...}, telemetry/op1a_*, builds.
- verify:op1-continuation was still running when the session died: it was waiting (from 01:36 to 03:17 UTC) for its
  second block `v2` (jobs5.txt: q10 / q9 / q5 / q6, queued 01:04) which never produced 4 status lines; it had also queued jobs6.txt (label `v1`, the 13 remaining queries, queued 01:36) to complete the suite - no results from either. Its note is
  complete except for that second block (note says "queued; see section 7"); it returned no final verdict text.
  Verdict in the note (section 0) stands on one block per query.
