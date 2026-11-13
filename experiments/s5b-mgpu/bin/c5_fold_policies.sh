#!/usr/bin/env bash
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# Stage C5: the three fold policies on 4 GPUs, TPC-H SF10 read from disk, at the E2-style settings
# max_build_hash_table_bytes = 0 and concat_batch_bytes = H/8 (H = resolved hash_partition_bytes).
#
#   bash experiments/s5b-mgpu/bin/c5_fold_policies.sh run [--retry-failed]
#   bash experiments/s5b-mgpu/bin/c5_fold_policies.sh summarize

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

parity() { s5b_py "$S5B_DIR/lib/parity_stage.py" "$@"; }
POLICIES=(never legacy_filter_coupled bounded)

case "${1:-}" in
  run)
    s5b_as_step "$(s5b_step_name C5 "$@")" 10800 "$0" "$@"
    shift
    status=0
    for policy in "${POLICIES[@]}"; do
      parity run --stage C5 --label "$policy" --build tip --gpus 4 --sf 10 "$@" \
        --set max_build_hash_table_bytes=0 --set concat_batch_bytes=H/8 \
        --set "build_fold_policy='$policy'" || status=1
    done
    exit "$status"
    ;;
  summarize)
    parity summarize --stage C5 --labels "$(IFS=,; echo "${POLICIES[*]}")" --resource-baseline never
    ;;
  *) s5b_die "usage: c5_fold_policies.sh run [--retry-failed] | summarize" ;;
esac
