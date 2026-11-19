# proto-single-gpu-build.patch  (branch proto/single-gpu-build, worktree R/wt-sgb; R=/opt/dlami/nvme/dfperf)

Recovery status: EXACT. Replayed from transcript wf_bbf271b6-7fa / agent a2eed8881c6a9884c (proto:single-gpu-build):
python in-place edits #32 #35 #38 #39 (commit 1), cherry-pick of mb/lifecycle (commit 2), python edits #49 #55 (commit 3).
Cross-checks: `git diff ae939824 HEAD -- src/` printed by the verifier (agent a3fad9246a2dd57ba, call #10, whole diff, 484 lines)
is line-identical to the recovered diff; `git diff --stat` identical (15 files, +1081/-16); bench file 837 lines and
lines 700-837 identical to the verifier's `sed` output. No uncommitted changes existed (git status clean at the last commit).
Not compiled after recovery. Submodules not initialised in the worktree.

Base: pr2570 @ ae939824. Commits (original -> recovered):
- c13359e5 -> 37cb0f01  proto(sgb): local Bloom replicas for broadcast builds (SIRIUS_PROTO_SGB=1)
                        (has a declaration-order compile error in accumulated_bloom_builder.hpp, fixed in commit 3, as in the original)
- 0ee6d8ae -> 3f3f0f6d  cherry-pick of mb/lifecycle (orig a5b37728, recovered b31ca131; see mb-lifecycle.md): lifecycle bench
                        MB0/MB2-MB5/MB7 + SIRIUS_PROTO_BLOOM_CHUNK_MIB + cmake registration
- d35151cf -> 28ad6e92  MB8 local-replica microbench; finish_local declaration order

Files in this store:
- proto-single-gpu-build.patch                    git diff pr2570 (everything, incl. the cherry-picked lifecycle bench)
- proto-single-gpu-build.over-mb-lifecycle.patch  git diff mb/lifecycle(b31ca131)..HEAD = the SGB prototype + MB8 only
- proto-single-gpu-build.commits.mbox             the 3 commits (git am)

What it does (details: notes/PROTO_SGB.md section 4): for a Bloom-only construction whose build PARTITION is in broadcast mode
with slots == replica GPUs, PARTITION tasks only claim their batch; each broadcast slot's whole build (CONCAT output) is inserted
into that GPU's own Bloom array at sirius_physical_hash_join::push_data_batch_partitioned("build")
(session.contribute_replica); the last replica publishes via accumulated_bloom_builder::finish_local (no merge, no cross-GPU copy).
Switch: env SIRIUS_PROTO_SGB=1 (read once; unset/other = tip behaviour). Debug log marker: "(PROTO local replicas)".
Bench: Catch2 "[mb8]" in sirius_unittest; env SIRIUS_BENCH_DF_GPUS, SIRIUS_BENCH_DF_MB, SIRIUS_BENCH_DF_REPS.
Known prototype gaps: session stays in PUBLISHING/local_await with no terminal handling (cancel/finalize); exact kinds not covered;
multi-batch slot delivery skips the filter (see notes/VERIFY_PROTO_SGB.md section 6).

Build / run (scripts/runs/sgb/bin, restored to R/runs/sgb/bin; verifier's in scripts/runs/vsgb/bin -> R/runs/vsgb/bin):
- git -C R/wt-sgb submodule update --init --recursive   (not done in recovery)
- flock R/machine.lock R/runs/sgb/bin/build.sh
- R/runs/sgb/bin/abba.sh t1 tip sgb 3 8 5 16 9 12 ; python3 R/runs/sgb/bin/tab.py tip sgb t1 ; R/runs/sgb/bin/resdiff.sh 3 5 8 9 12 16
- flock L R/runs/sgb/bin/mb.sh g4 0,1,2,3 "[mb8]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5
- oneq.sh sides: tip (R/repo) | base (R/wt-base) | sgb (wt-sgb, env on) | sgboff (wt-sgb, env off); EXTRA="--precmd nsys --query-timeout 3600" for nsys
- scripts depend on files owned by other groups: runs/profile/bin/{qload.py,nsys_df.py,quent_df.py}, /home/ubuntu/sirius/specific_config.yaml, R/wt-base.

Script status: all EXACT (heredoc replay; sed -i edits of oneq.sh/probe.py replayed; abba/build/oneq/resdiff/tl/probe confirmed
against later `cat` output). runs/sgb/bin/mb.sh was made by `sed` from mblife/run.sh: taken verbatim from the verifier's `cat` (EXACT).
Not recovered (lost for good): raw runs (runs/sgb/*, runs/vsgb/*), telemetry/sgb, telemetry/vsgb, nsys captures, build logs.
