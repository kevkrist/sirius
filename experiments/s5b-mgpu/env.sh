# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# Shared settings and helpers for the S5b multi-GPU experiment scripts. Every stage script sources
# this file. Machine-specific values belong in `experiments/s5b-mgpu/local.env` (untracked; copy
# `local.env.example`), which is read first so that its values win over the defaults below.
#
# This file only defines variables and helpers; it runs nothing.

# shellcheck shell=bash

S5B_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
S5B_CONTROL="$(cd "$S5B_DIR/../.." && pwd)"
if [[ -f "$S5B_DIR/local.env" ]]; then
  # shellcheck source=/dev/null
  source "$S5B_DIR/local.env"
fi

# The stack under test (spec rev 3.3). BASE is resolved to a full SHA after the fetch.
S5B_TIP_COMMIT="${S5B_TIP_COMMIT:-b65efbe3}"
S5B_KEY_COMMIT="${S5B_KEY_COMMIT:-2b3b1c69}"
S5B_BASE_COMMIT="${S5B_BASE_COMMIT:-2b3b1c69^}"
S5B_STACK_BRANCHES=(stacked/df/05b-construction stacked/df/05b-switch stacked/df/05b-fold-rule)
S5B_UPSTREAM_REMOTE="${S5B_UPSTREAM_REMOTE:-origin}"
S5B_FORK_REMOTE="${S5B_FORK_REMOTE:-kevkrist}"
S5B_EXPERIMENT_BRANCH="${S5B_EXPERIMENT_BRANCH:-tmp/s5b-mgpu-experiments}"

# Where the build worktrees, data and raw run output live. Raw output never goes under results/.
S5B_HOME="${S5B_HOME:-$HOME/s5b-mgpu}"
S5B_DATA="${S5B_DATA:-$S5B_HOME/data}"
S5B_WORK="${S5B_WORK:-$S5B_HOME/work}"
S5B_RESULTS="${S5B_RESULTS:-$S5B_DIR/results}"
S5B_CONFIGS="${S5B_CONFIGS:-$S5B_WORK/configs}"
S5B_QUERIES="${S5B_QUERIES:-$S5B_WORK/queries}"
S5B_STEPS="${S5B_STEPS:-$S5B_WORK/steps}"
# The machine's own Sirius config for 4 GPUs, and the existing TPC-H SF1000 Parquet directory.
# S0 asks the user for both once; they go in local.env.
S5B_SIRIUS_CONFIG="${S5B_SIRIUS_CONFIG:-}"
S5B_TPCH_SF1000="${S5B_TPCH_SF1000:-}"

# One build worktree per commit. The names tip, key and base are used everywhere.
S5B_WT_tip="${S5B_WT_tip:-$S5B_HOME/wt-tip}"
S5B_WT_key="${S5B_WT_key:-$S5B_HOME/wt-key}"
S5B_WT_base="${S5B_WT_base:-$S5B_HOME/wt-base}"
S5B_BUILDS=(tip key base)

# Paths the kit created; bin/cleanup.sh deletes nothing else (format and rules: lib/manifest.py).
S5B_MANIFEST="$S5B_DIR/.created-paths"

s5b_log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
s5b_die() {
  s5b_log "ERROR: $*"
  exit 1
}

# The worktree of build $1 (tip, key or base).
s5b_worktree() {
  local name="S5B_WT_$1"
  [[ -n "${!name:-}" ]] || s5b_die "unknown build '$1' (expected one of: ${S5B_BUILDS[*]})"
  printf '%s\n' "${!name}"
}

# The commit of build $1, as given (not resolved).
s5b_commit() {
  case "$1" in
    tip) printf '%s\n' "$S5B_TIP_COMMIT" ;;
    key) printf '%s\n' "$S5B_KEY_COMMIT" ;;
    base) printf '%s\n' "$S5B_BASE_COMMIT" ;;
    *) s5b_die "unknown build '$1'" ;;
  esac
}

# Runs "$@" through pixi in worktree $1 without any inherited SIRIUS_* or CUDA_VISIBLE_DEVICES
# variable, so the command sees that worktree's activated environment and nothing stray.
s5b_pixi_in() {
  local worktree="$1" unset_args=() name
  shift
  for name in $(compgen -e); do
    if [[ "$name" == SIRIUS_* || "$name" == CUDA_VISIBLE_DEVICES ]]; then
      unset_args+=(-u "$name")
    fi
  done
  (cd "$worktree" && env "${unset_args[@]}" pixi run "$@")
}

# Runs a Python script of this directory with the tip worktree's environment (pyarrow, pyyaml).
# The base, key and tip worktrees share one pixi.lock (checked in S0), so any of them would do.
s5b_py() {
  [[ -d "$S5B_WT_tip/.pixi" ]] || s5b_die "the tip worktree has no pixi environment yet; finish S0 'build' first"
  s5b_pixi_in "$S5B_WT_tip" python -I "$@"
}

# Appends "<kind> TAB <absolute path> TAB <time>" to the manifest of created paths.
s5b_record_created() {
  printf '%s\t%s\t%s\n' "$1" "$(cd "$2" && pwd -P)" "$(date -Is)" >> "$S5B_MANIFEST"
}

# Creates S5B_WORK when missing, marks it as the kit's own and records it for cleanup.
s5b_ensure_work() {
  if [[ ! -e "$S5B_WORK" ]]; then
    mkdir -p "$S5B_WORK"
    : > "$S5B_WORK/.s5b-work"
    s5b_record_created work "$S5B_WORK"
  fi
  mkdir -p "$S5B_STEPS"
}

# Refuses when an engine or a step is still running: a duckdb or Sirius test process, a GPU
# compute process, or a step whose PID file has no done marker and whose process is alive.
s5b_guard_idle() {
  local found="" pids apps pidfile pid
  pids="$(pgrep -a -x duckdb || true)"
  [[ -z "$pids" ]] || found+=$'\n'"duckdb: $pids"
  pids="$(pgrep -a -x sirius_unittest || true)"
  [[ -z "$pids" ]] || found+=$'\n'"sirius_unittest: $pids"
  pids="$(pgrep -a -f 'scripts/run_unit_tests[.]py' || true)"
  [[ -z "$pids" ]] || found+=$'\n'"unit-test harness: $pids"
  apps="$(nvidia-smi --query-compute-apps=pid,process_name --format=csv,noheader 2> /dev/null || true)"
  [[ -z "$apps" ]] || found+=$'\n'"GPU processes: $apps"
  if [[ -d "$S5B_STEPS" ]]; then
    for pidfile in "$S5B_STEPS"/*.pid; do
      [[ -f "$pidfile" && ! -f "${pidfile%.pid}.done" ]] || continue
      pid="$(cat "$pidfile")"
      if kill -0 "$pid" 2> /dev/null; then
        found+=$'\n'"step $(basename "${pidfile%.pid}") (pid $pid) is running"
      fi
    done
  fi
  if [[ -n "$found" ]]; then
    s5b_die "refusing to start: something is still running.${found}"$'\n'"Do not kill processes you did not start; STOP and ask the user."
  fi
}

# Runs the calling script as one step, unless it already runs as one (S5B_IN_STEP is set):
#   s5b_as_step <name> <timeout_s> "$0" "$@"
# The step refuses to start next to a running engine or step, writes $S5B_STEPS/<name>.pid, runs
# `bash "$0" "$@"` under a hard `timeout` (exit 124 when it fires; its whole process group is
# killed), tees the output to $S5B_STEPS/<name>.out, and writes the exit code to
# $S5B_STEPS/<name>.done. S5B_STEP_TIMEOUT overrides the limit.
s5b_as_step() {
  [[ -z "${S5B_IN_STEP:-}" ]] || return 0
  local name="$1" limit="${S5B_STEP_TIMEOUT:-$2}" status pid
  shift 2
  s5b_ensure_work
  s5b_guard_idle
  local base="$S5B_STEPS/$name"
  rm -f "$base.done"
  s5b_log "step $name: limit ${limit}s; output $base.out; exit code goes to $base.done"
  S5B_IN_STEP="$name" timeout --kill-after=120 "$limit" bash "$@" > >(tee -a "$base.out") 2>&1 &
  pid=$!
  printf '%s\n' "$pid" > "$base.pid"
  status=0
  wait "$pid" || status=$?
  printf '%s\n' "$status" > "$base.done"
  ((status != 124)) || s5b_log "step $name: TIMED OUT after ${limit}s"
  s5b_log "step $name: exit $status"
  exit "$status"
}

# A step name from words: P1 run --gpus 4 -> P1-run-gpus-4.
s5b_step_name() {
  printf '%s\n' "$*" | tr -c 'A-Za-z0-9,._\n' '-' | tr -s '-' | sed 's/-$//'
}

# Exports the variables the Python scripts read, so they agree with this file.
s5b_export() {
  export S5B_DIR S5B_CONTROL S5B_HOME S5B_DATA S5B_WORK S5B_RESULTS S5B_CONFIGS S5B_QUERIES
  export S5B_STEPS S5B_SIRIUS_CONFIG S5B_TPCH_SF1000
  export S5B_WT_tip S5B_WT_key S5B_WT_base
  export S5B_TIP_COMMIT S5B_KEY_COMMIT S5B_BASE_COMMIT
  local name
  for name in $(compgen -v S5B_TPCH_SF); do export "${name?}"; done
}
s5b_export
