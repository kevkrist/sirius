#!/usr/bin/env bash
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# Stage C2 (with C3 folded in): Key Outcomes on TPC-H SF1 read from disk, 4 GPUs and 2 GPUs.
#
#   bash experiments/s5b-mgpu/bin/c2_key_outcomes.sh run [--retry-failed] [4|2 ...]   # default: 4 then 2
#   bash experiments/s5b-mgpu/bin/c2_key_outcomes.sh summarize
#
# Per GPU count it runs, one query process at a time:
#   key_<n>gpu         baseline 2b3b1c69 (S5 tip plus the Key Outcome line)
#   tip_<n>gpu         tip b65efbe3
#   tip_df_off_<n>gpu  tip with enable_dynamic_filter = false
# then diffs key -> tip. Resumable: rerun the same command after an interruption.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

parity() { s5b_py "$S5B_DIR/lib/parity_stage.py" "$@"; }

# run_gpus <n> [--retry-failed]
run_gpus() {
  local n="$1" status=0
  shift
  parity run --stage C2 --label "key_${n}gpu" --build key --gpus "$n" --sf 1 "$@" || status=1
  parity run --stage C2 --label "tip_${n}gpu" --build tip --gpus "$n" --sf 1 "$@" || status=1
  parity run --stage C2 --label "tip_df_off_${n}gpu" --build tip --gpus "$n" --sf 1 "$@" \
    --set enable_dynamic_filter=false || status=1
  parity diff --stage C2 --base "key_${n}gpu" --candidate "tip_${n}gpu" || status=1
  return "$status"
}

case "${1:-}" in
  run)
    s5b_as_step "$(s5b_step_name C2 "$@")" 7200 "$0" "$@"
    shift
    retry=()
    if [[ "${1:-}" == --retry-failed ]]; then
      retry=(--retry-failed)
      shift
    fi
    gpus=("$@")
    ((${#gpus[@]})) || gpus=(4 2)
    status=0
    for n in "${gpus[@]}"; do run_gpus "$n" "${retry[@]}" || status=1; done
    exit "$status"
    ;;
  summarize)
    labels=()
    diffs=()
    for n in 4 2; do
      labels+=("key_${n}gpu" "tip_${n}gpu" "tip_df_off_${n}gpu")
      diffs+=(--diff "key_${n}gpu:tip_${n}gpu")
    done
    parity summarize --stage C2 --labels "$(IFS=,; echo "${labels[*]}")" "${diffs[@]}" \
      --info-labels key_4gpu,key_2gpu
    ;;
  *) s5b_die "usage: c2_key_outcomes.sh run [--retry-failed] [4|2 ...] | summarize" ;;
esac
