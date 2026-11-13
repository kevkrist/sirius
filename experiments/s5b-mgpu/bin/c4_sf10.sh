#!/usr/bin/env bash
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# Stage C4: TPC-H SF10 read from disk, 4 GPUs, at the tip; results against the CPU and the Key
# Outcome kinds.
#
#   bash experiments/s5b-mgpu/bin/c4_sf10.sh run [--retry-failed]
#   bash experiments/s5b-mgpu/bin/c4_sf10.sh summarize

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

parity() { s5b_py "$S5B_DIR/lib/parity_stage.py" "$@"; }

case "${1:-}" in
  run)
    s5b_as_step "$(s5b_step_name C4 "$@")" 5400 "$0" "$@"
    shift
    parity run --stage C4 --label tip_4gpu --build tip --gpus 4 --sf 10 "$@"
    ;;
  summarize) parity summarize --stage C4 --labels tip_4gpu ;;
  *) s5b_die "usage: c4_sf10.sh run [--retry-failed] | summarize" ;;
esac
