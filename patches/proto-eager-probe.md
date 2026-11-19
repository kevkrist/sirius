# proto/eager-probe (worktree wt-eager) - S6 early probe activation, OPTION 1

Recovery status: EXACT. Replayed from the transcript of agent `proto:eager-probe`
(wf_925b413d-4e2/agent-a99dfa994a289082c.jsonl): python in-place edit (call 19) + `sed -i` include fix (call 49).
Cross-checks that passed: diffstat 3 files / +67 (same as original commit and as PROTO_EAGER.md section 1); the original
compile error positions (hash_join.cpp:966:64 / 967:12 before the include line was added) match the recovered text.

- Base: pr2570 = ae939824. Original commit 092a0e3a; recovered commit f05cfd12 (local, `/opt/dlami/nvme/dfperf/wt-eager`).
- Patch: `proto-eager-probe.patch` (= `git diff pr2570`), `proto-eager-probe.commits.mbox` (format-patch).
  `git apply --check` on pr2570: OK. Not compiled after recovery.
- Files: `src/op/partition/consumer_partition_policy.cpp` (finalize listener on the build PARTITION in
  `bind_partition_inputs`, unless the probe drives), `src/op/sirius_physical_hash_join.{hpp,cpp}`
  (`activate_probe_after_build_partition(build_concat) noexcept`, `_eager_probe_activated`, include `creator/task_creator.hpp`).
- Switches (env, read once per process):
  - `SIRIUS_EAGER_PROBE`: 0/unset = off (tip behaviour), 1 = joins with an enabled filter plan, 2 = every BUILD_PROBE join.
  - `SIRIUS_EAGER_CONCAT_FIRST` (default 1): nominate the build CONCAT before the probe head (0 made no difference).
- Build: `flock $R/machine.lock $R/runs/eager/bin/build.sh` (pixi run make in wt-eager; log `$R/logs/eager_build.log`;
  needs `git submodule update --init --recursive` + `pixi install` in the worktree first).
- Run: `$R/runs/eager/bin/` - `one_query.sh <label> <q> <iters>` (env REPO, SIRIUS_EAGER_PROBE, ...), `unit.sh <tag> <iters> <q> <arm...>`
  (arms: tip, pro=eager 1, pro0, pro2=eager 2, pros=concat_first 0; o1/o2/ob/t1/t2/o2t1 use wt-eager2), `smoke.sh` +
  `smoke_check.py` (GO/NOGO), `gated.sh`, `queue_a1.sh` (batch a1), `queue_logpass.sh`, analysis `times.py`, `arms.py`,
  `esites.py`, `cmp_results.py`, `cmp_df.py`, `cmp_decode.py`.
- Finished when the session died: everything for option 1 - smoke GO, batch a1 (22 queries, 98 processes), TRACE logpass,
  notes PROTO_EAGER.md sections 1-5, 7, 8, LOGPASS. Result: suite -295 ms (-1.9%) vs tip. No verifier had started
  (no VERIFY_PROTO_EAGER_*.md existed).
