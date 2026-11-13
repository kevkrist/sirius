#!/usr/bin/env bash
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# The state of the kit's steps (env.sh `s5b_as_step`), for the session that watches them.
#
#   bash experiments/s5b-mgpu/bin/steps.sh status          # every step: running, exit code, or killed
#   bash experiments/s5b-mgpu/bin/steps.sh tail <name>     # last 40 lines of a step's output
#   bash experiments/s5b-mgpu/bin/steps.sh guard           # exit 1 if an engine or step is running

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

case "${1:-status}" in
  status)
    [[ -d "$S5B_STEPS" ]] || { echo "no steps yet"; exit 0; }
    for pidfile in "$S5B_STEPS"/*.pid; do
      [[ -f "$pidfile" ]] || continue
      base="${pidfile%.pid}"
      name="$(basename "$base")"
      if [[ -f "$base.done" ]]; then
        code="$(cat "$base.done")"
        state="exit $code"
        [[ "$code" != 124 ]] || state="exit 124 (timed out)"
      elif kill -0 "$(cat "$pidfile")" 2> /dev/null; then
        state="running (pid $(cat "$pidfile"))"
      else
        state="ended without a done marker (killed?)"
      fi
      printf '%-40s %s\n' "$name" "$state"
    done
    ;;
  tail) tail -40 "$S5B_STEPS/${2:?usage: steps.sh tail <name>}.out" ;;
  guard) s5b_guard_idle && echo "nothing running" ;;
  *) s5b_die "usage: steps.sh status | tail <name> | guard" ;;
esac
