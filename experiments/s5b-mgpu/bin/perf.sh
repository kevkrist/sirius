#!/usr/bin/env bash
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# Stages P1 (with P4), P2 and P3. Run with plain bash; `reference` and `run` are steps (env.sh
# `s5b_as_step`: PID file, done marker, hard timeout).
#
#   bash experiments/s5b-mgpu/bin/perf.sh <P1|P2|P3> reference          # CPU answers, once per SF
#   bash experiments/s5b-mgpu/bin/perf.sh <P1|P2|P3> plan [bench args]  # sessions, runs, estimate
#   bash experiments/s5b-mgpu/bin/perf.sh <P1|P2|P3> run [bench args]   # resumable per session
#   bash experiments/s5b-mgpu/bin/perf.sh <P1|P2|P3> summarize [bench args]
#
# Bench args (see `lib/bench.py --help`), for example
#   P1: --gpus 4        P3: --policies never,bounded        any: --retry-failed

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

usage="usage: perf.sh <P1|P2|P3> <reference|plan|run|summarize> [args]"
stage="${1:?$usage}"
command="${2:?$usage}"

case "$stage" in
  P1 | P3) sf=1000 limit=$((40 * 3600)) ;;
  P2) sf=100 limit=$((20 * 3600)) ;;
  *) s5b_die "unknown stage '$stage'" ;;
esac

case "$command" in
  reference) s5b_as_step "$(s5b_step_name "$@")" $((10 * 3600)) "$0" "$@" ;;
  run) s5b_as_step "$(s5b_step_name "$@")" "$limit" "$0" "$@" ;;
esac
shift 2

case "$command" in
  reference) s5b_py "$S5B_DIR/lib/cpu_reference.py" --sf "$sf" --stage "$stage" "$@" ;;
  plan | run | summarize) s5b_py "$S5B_DIR/lib/bench.py" "$command" --stage "$stage" "$@" ;;
  *) s5b_die "unknown command '$command'" ;;
esac
