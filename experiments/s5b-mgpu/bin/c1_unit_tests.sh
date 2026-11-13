#!/usr/bin/env bash
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# Stage C1: unit tests at the tip, in the tip worktree. Run with plain bash; every step is a step
# (env.sh `s5b_as_step`: PID file, done marker, hard timeout).
#
#   bash experiments/s5b-mgpu/bin/c1_unit_tests.sh multi_gpu        # [multi_gpu] with 4 GPUs
#   bash experiments/s5b-mgpu/bin/c1_unit_tests.sh multi_gpu_peer   # same, peer-copy tests may not skip
#   bash experiments/s5b-mgpu/bin/c1_unit_tests.sh full             # pixi run make test, 8 shard processes
#
# Before a step, the previous logs under the tip worktree's build/release/test/cpp/log/ are moved
# to S5B_WORK/C1/stale-<time>/, so a step's excerpts come only from its own processes. Each step
# keeps a short console tail, the Catch2 summary lines and the skip/failure excerpts under
# results/C1/<step>/.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

C1_RESULTS="$S5B_RESULTS/C1"
C1_WORK="$S5B_WORK/C1"
LOG_ROOT="$S5B_WT_tip/build/release/test/cpp/log"

collect() {
  local step="$1" status="$2" console="$3"
  local out="$C1_RESULTS/$step"
  mkdir -p "$out"
  {
    echo "step=$step exit=$status recorded=$(date -Is) commit=$(git -C "$S5B_WT_tip" rev-parse HEAD)"
    echo
    sed -n '/^Unit test summary/,$p' "$console"
  } > "$out/summary.txt"
  : > "$out/catch2_totals.txt"
  : > "$out/skips_and_failures.txt"
  local dir name
  for dir in "$LOG_ROOT"/*/; do
    [[ -f "$dir/unittest.log" ]] || continue
    name="$(basename "$dir")"
    grep -E '^(All tests passed|test cases:|assertions:|No tests ran)' "$dir/unittest.log" |
      sed "s/^/$name: /" >> "$out/catch2_totals.txt" || true
    # Catch2 prints a block per skipped or failed test case; keep the test name lines and reasons.
    grep -nE -B2 -A6 'SKIPPED|FAILED|explicitly skipped|SKIP\(' "$dir/unittest.log" |
      head -400 | sed "s/^/$name: /" >> "$out/skips_and_failures.txt" || true
  done
  tail -60 "$console" > "$out/console_tail.txt"
  s5b_log "C1 $step exit $status; excerpts in $out"
  cat "$out/summary.txt" >&2
}

run_step() {
  local step="$1"
  shift
  mkdir -p "$C1_WORK"
  if [[ -d "$LOG_ROOT" ]]; then
    mv "$LOG_ROOT" "$C1_WORK/stale-$(date +%Y%m%d-%H%M%S)"
  fi
  local console="$C1_WORK/$step.console.txt"
  local status=0
  s5b_pixi_in "$S5B_WT_tip" "$@" > "$console" 2>&1 || status=$?
  collect "$step" "$status" "$console"
  return "$status"
}

step="${1:-}"
case "$step" in
  multi_gpu | multi_gpu_peer) s5b_as_step "C1-$step" 3600 "$0" "$@" ;;
  full) s5b_as_step "C1-$step" 10800 "$0" "$@" ;;
  *) s5b_die "usage: c1_unit_tests.sh multi_gpu|multi_gpu_peer|full" ;;
esac
case "$step" in
  multi_gpu)
    run_step multi_gpu python scripts/run_unit_tests.py --steps multi_gpu --require-gpus 4
    ;;
  multi_gpu_peer)
    run_step multi_gpu_peer env SIRIUS_TEST_REQUIRE_POOL_PEER_ACCESS=1 \
      python scripts/run_unit_tests.py --steps multi_gpu --require-gpus 4
    ;;
  full)
    # make test runs the shards (8 processes: 2 per GPU, in parallel), then cuda_fault, multi_gpu
    # and late_mat; cuda_fault and late_mat each run two jobs of their own.
    run_step full make test UNITTEST_ARGS="--require-gpus 4 --shards 8"
    ;;
esac
