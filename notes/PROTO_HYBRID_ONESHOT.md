# PROTO_HYBRID_ONESHOT: PARTITION construction for multi-partition builds + ONE-SHOT for whole-build deliveries?

2026-11-27. Worktree `/opt/dlami/nvme/dfperf/wt-hybrid`, branch `proto/hybrid-oneshot` (from pr2570 `ae939824`).
Scripts `$R/runs/hybrid/bin/`, raw `$R/runs/hybrid/<label>/`, Quent `$R/telemetry/hybrid_<label>/q<N>/`.
Every build / GPU process under `flock $R/machine.lock`. Config untouched. Nothing pushed to sirius-db.
STATUS: IN PROGRESS (see checkpoint log at the end).

(sections are filled as work completes; see CHECKPOINT LOG)

## CHECKPOINT LOG (chronological; newest last)

- 19:05 worktree + branch created; submodules need `git -c protocol.file.allow=always submodule update --init --recursive <paths>`.
- 19:15 prototype written (commit 7d7cdcdb): pr2569 `observe_whole_build` + `publish_dynamic_filters` ported verbatim into the
  tip `dynamic_filter_publisher.cpp`; `hash_join::push_data_batch_partitioned` override + `set_build_arrives_whole` restored;
  observer defers (no begin/decline, no finalize) when the switch is on and the build arrives whole.
  Switch `SIRIUS_PROTO_ONESHOT`: 0 tip; 1 one-shot, publish THEN deposit; 2 one-shot, deposit first (pre-stack order).
  `proto/eager-probe` cherry-picked (618fc1c8) so the same binary has `SIRIUS_EAGER_PROBE`. Build queued behind the lock.
