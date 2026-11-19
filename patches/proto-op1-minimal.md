# proto/op1-minimal (worktree `wt-op1b`) — recovered

Recovery status: **EXACT**. Rebuilt by replaying the original agent's in-place Python edit scripts, in order, on a
fresh `pr2570` (`ae939824`, same tip the prototype was based on). Checks against the transcript:
- commit 1 `git diff --stat`: 11 files, +247 -8 (original: same); final: 12 files, +253 -8 (original: same)
- line-for-line equal to the original `git diff` of `sirius_physical_partition.cpp`, `gpu_pipeline_task.cpp`,
  `sirius_physical_hash_join.cpp` printed in the transcript
- No hunk missing, none inferred. Not compiled after recovery (original built and ran).

Files: `proto-op1-minimal.patch` (= `git diff pr2570 proto/op1-minimal`), `proto-op1-minimal.commits.mbox`
(the 2 commits, `git am`-able).

## Commits (original hash -> recovered hash in `/opt/dlami/nvme/dfperf/wt-op1b`)
1. `cc1c225b` -> `b7d97426` proto(op1b): publish dynamic filters after the Publishing Task's deposit
   (11 files under `src/op/dynamic_filter/`, `src/op/partition/`, `src/op/`, `src/pipeline/`)
2. `e6cec1c3` -> `37850ed0` proto(op1b): SIRIUS_PROTO_NUM_GPUS measurement override (`src/sirius_config.cpp`)

Recovered commit messages are prefixed `recovered prototype: `; author is `proto <proto@local>` as in the original.

## What it does
Throwaway prototype of OPPORTUNITIES OP1 (minimal form). The elected PARTITION Publishing Task deposits first, claims
its publication, reports itself completed to the pipeline (so the build CONCAT starts), then runs the unchanged
finish / replicate / fan-out on its own stream and reservation. The hash join waits for the deferred publication
(`await_deferred_publication`, condition variable) before its hint names the probe producer. Details: section 3 of
`notes/PROTO_OP1B.md`.

## Switches (env vars, read at run time)
- `SIRIUS_DF_DEFER_PUBLISH`: `0` = stack-tip behaviour (publish before deposit); `1` = default (deferred);
  `2` = also every non-publishing contributing task reports completion right after its deposit.
- `SIRIUS_PROTO_NUM_GPUS=N`: measurement only, run on the first N GPUs without editing the config.
- Log lines tagged `[op1b]` (INFO) give deferred-publication duration and join-point wait; parsed by `op1b_log.py`.

## Build / run
- Worktree needs `git submodule update --init --recursive` first (not done in recovery).
- Build: `flock $R/machine.lock $R/runs/op1b/bin/build.sh` (`pixi run make`, log `$R/logs/op1b_build.log`).
- Timing: `runs/op1b/bin/abba_locked.sh <tag> <iters> <q...>` (one lock hold per query; arms tip / pro / pro0 / pro2 /
  g2pro0 / g2pro), then `runs/op1b/bin/times.py <A labels> <B labels> <q...>`.
- Verifier's set: `runs/vop1b/bin/` (`arms.sh`, `logpass.sh`, `unit.sh`, `edge.sh` + `edge.py`).

## Outcome (numbers in the notes)
Proto: 13 queries measured, -168 ms (-1.1%), gain only where transfers are HOST-staged. Verifier: confirmed, verdict
**drop for the stack** (zero gain on the P2P pair). See `notes/PROTO_OP1B.md`, `notes/VERIFY_PROTO_OP1B.md`.
