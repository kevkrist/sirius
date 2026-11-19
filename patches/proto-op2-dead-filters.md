# proto/op2-dead-filters (OP2 "shadow rule" + consumer-gate debug logging + LATCH/ENDPOINT/oracle switches)

Recovered 2026-11-19 from agent transcripts (workflow wf_5c374f09-bb4, agents `proto:op2-dead-filters`
a4e04a48ddcf05515 and `verify:op2-dead-filters` a7c8baad6e4bc805c) after the NVMe volume was wiped.

## Recovery status: EXACT (all 5 files)

- Base: pr2570 `ae939824`. All code edits were made by Python `assert count==1; replace` heredocs and one `cat > file <<EOF`;
  they were replayed verbatim, in order, on a fresh pr2570 worktree; every assert passed.
- Proof: the verifier's `git diff ae939824..HEAD` outputs in the transcript carry the post-image blob ids; the recovered tree
  has the same ids: `dynamic_filter_publisher.cpp` 6a7a5b59, `sirius_dynamic_filter.hpp` d4767ed9,
  `dynamic_filter_merge.cpp` e9deaee5, `sirius_gpu_scan_operator_data.cpp` f3048c8b. `proto_op2.hpp` (new file, blob e5edae2f)
  equals the verifier's `cat` of it byte for byte. Diffstat equal: 5 files, +335 / -6.
- No hunk is missing or inferred. The verifier did not modify the prototype.
- Only difference from the lost branch: commit ids and the message prefix `recovered prototype: `.

## Files

- `proto-op2-dead-filters.patch`: `git diff pr2570` (apply with `git apply` on pr2570; `git apply --check` passes).
- `proto-op2-dead-filters.commits.mbox`: the 3 commits (`git am`), same split as the original branch.
- Restored worktree: `/opt/dlami/nvme/dfperf/wt-op2`, branch `proto/op2-dead-filters`, local only, submodules NOT initialised, not built.

## Commits (original id -> recovered id)

1. d35058cb -> 8a18c918 `proto(op2): DEBUG logging of consumer gate state per filter (decode + endpoint)`
   `src/op/scan/dynamic_filter_merge.cpp`, `src/op/scan/sirius_gpu_scan_operator_data.cpp`: `[df_gate]` DEBUG lines
   (endpoint apply/skip + reason, decode attach/skip, decode-probe rows, decode-probe DECLINED, gate state transitions).
2. 6ebc0a98 -> 29889fbb `proto(op2): env-gated switches: shadow skip, oracle skip, latch fix, endpoint apply, OP3 oracle sizing`
   new `src/op/dynamic_filter/proto_op2.hpp`; `dynamic_filter_publisher.cpp` (shadow / oracle decision before
   `plan_construction`, `set_estimate_reliable`, OP3 `fixed_bloom_keys` override, `[df_proto]` DEBUG lines);
   `sirius_dynamic_filter.hpp` (`estimate_reliable` flag); `dynamic_filter_merge.cpp` (ENDPOINT);
   `sirius_gpu_scan_operator_data.cpp` (LATCH).
3. 523ae08c -> 41d36d9d `proto(op2): keep diagnostics free at INFO` (`describe_filter` returns early unless DEBUG is on).

## Switches (env vars, read once per process, all off by default)

| env | effect |
|---|---|
| `SIRIUS_PROTO_OP2_SHADOW=1` | shadow rule: at construction begin skip a key when every binding would be dropped by the endpoint's negligible-removed-share rule (`survival < 1.0 && survival * (1 - own) < 0.10`), judged against filters already on the target channel, reliable (unique-key, `domain_gate_eligible`) estimates only. Production candidate. |
| `SIRIUS_PROTO_OP2_LATCH=1` | a decode that gave compaction up while every probe it carried was weak does not set `pushdown_selection_unprofitable` |
| `SIRIUS_PROTO_OP2_ENDPOINT=1` | endpoint rule ignores a stronger filter the decode already applied ("make it useful" test; verdict: drop) |
| `SIRIUS_PROTO_OP2_SKIP=join:condition,...` | oracle: do not build that key's filters (q9: `29:1`) |
| `SIRIUS_PROTO_OP3_BLOOM_KEYS=join:keys,...` | oracle Bloom sizing for a join (q9: `29:10870459`; verdict: drop) |
| `SIRIUS_LOG_LEVEL=debug` | needed to see `[df_gate]` / `[df_proto]` lines |

## Build / run

- Build: `git -C wt-op2 submodule update --init --recursive`, then `flock $R/machine.lock $R/runs/op2/bin/build.sh`
  (`pixi run make`, log `$R/logs/build_op2.log`).
- One query: `[env] REPO=$R/wt-op2 flock $R/machine.lock $R/runs/op2/bin/one_query.sh <label> <q> <iters>` (REPO=$R/repo = tip).
- Palindromic A/B: `$R/runs/op2/bin/pal.sh <tag> 6 "<q...>" tip "shadow:SIRIUS_PROTO_OP2_SHADOW=1" ...`; table `pal_table.py <tag> <variant...>`.
- Gate log summary: `gate_summary.sh <sirius log>`. DF unit tests: `unit.sh` (switches off), verifier `runs/vop2/bin/ut.sh` (off + on).
- Verifier harness `$R/runs/vop2/bin/`: `unit.sh` (one locked ABBA unit; variants tip/off/sh/la/shl/sk9/ep/tip2/sh2),
  `dbg.sh`, `next.sh` (queue popper over `todo.txt`), `vtable.py`, `logdiff.py`, `fire.py`, `qsum.py`, `rowdiff.py`,
  `p2p.sh` / `p2p_off.sh` (these two need the OP1A worktree `$R/wt-op1a` for `SIRIUS_PROTO_NUM_GPUS`).
- Results and verdict: `notes/PROTO_OP2.md`, `notes/VERIFY_PROTO_OP2.md`.

## Not recovered (lost for good)

Raw run output (`runs/op2/*`, `runs/vop2/*` label dirs, `todo.txt` / `done.txt` queue state, `quent/*.json`), telemetry, builds.
Scripts depend on `runs/profile/bin/quent_df.py` and `runs/baseline/logpass` (other groups / lost data).
