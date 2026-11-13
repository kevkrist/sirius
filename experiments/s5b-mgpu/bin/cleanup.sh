#!/usr/bin/env bash
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# Final cleanup. Run ONLY when the user says so, and only the parts they name, in this order.
#
#   bash experiments/s5b-mgpu/bin/cleanup.sh work            # delete raw run output (S5B_WORK)
#   bash experiments/s5b-mgpu/bin/cleanup.sh worktrees       # remove the tip, key and base worktrees
#   bash experiments/s5b-mgpu/bin/cleanup.sh remote-branch   # delete the experiment branch on the fork
#
# `work` and `worktrees` delete only paths the kit recorded as created and that contain no data,
# checkout or home directory (lib/manifest.py checks; it runs in the tip worktree's environment,
# so run `work` before `worktrees`). The TPC-H data is never deleted. Every part asks for the
# literal word "yes" on stdin first.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

confirm() {
  local answer
  read -r -p "$1 Type yes to continue: " answer
  [[ "$answer" == yes ]] || s5b_die "cancelled"
}

guard() { s5b_py "$S5B_DIR/lib/manifest.py" check-delete "$1" "$2" || s5b_die "not deleting $2"; }

case "${1:-}" in
  work)
    s5b_guard_idle
    guard work "$S5B_WORK"
    confirm "Delete raw run output under $S5B_WORK?"
    rm -rf -- "${S5B_WORK:?}"
    ;;
  worktrees)
    s5b_guard_idle
    targets=()
    for build in "${S5B_BUILDS[@]}"; do
      wt="$(s5b_worktree "$build")"
      [[ -d "$wt" ]] || continue
      guard worktree "$wt"
      [[ -z "$(git -C "$wt" status --porcelain --untracked-files=no)" ]] || s5b_die "$wt has local changes"
      targets+=("$wt")
    done
    ((${#targets[@]})) || s5b_die "no worktrees to remove"
    confirm "Remove worktrees ${targets[*]} (with their builds)?"
    for wt in "${targets[@]}"; do
      git -C "$S5B_CONTROL" worktree remove --force "$wt"
      s5b_log "removed $wt"
    done
    git -C "$S5B_CONTROL" worktree prune
    ;;
  remote-branch)
    url="$(git -C "$S5B_CONTROL" remote get-url "$S5B_FORK_REMOTE")"
    [[ "$url" != *sirius-db/sirius* ]] || s5b_die "refusing: $S5B_FORK_REMOTE points at $url"
    confirm "Delete $S5B_EXPERIMENT_BRANCH on $S5B_FORK_REMOTE ($url)?"
    git -C "$S5B_CONTROL" push "$S5B_FORK_REMOTE" --delete "$S5B_EXPERIMENT_BRANCH"
    ;;
  *) s5b_die "usage: cleanup.sh work|worktrees|remote-branch" ;;
esac
