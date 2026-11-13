#!/usr/bin/env bash
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
#
# Shares one stage's small results with the user through their fork. Never pushes to origin.
#
#   bash experiments/s5b-mgpu/bin/publish_results.sh show <stage>    # what would be committed
#   bash experiments/s5b-mgpu/bin/publish_results.sh commit <stage>  # after the user approves
#   bash experiments/s5b-mgpu/bin/publish_results.sh push            # after the user approves
#
# Commits use the Conventional Commits type `chore` and carry no co-author or attribution line.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

MAX_FILE_BYTES=$((5 * 1024 * 1024))

stage_path() {
  local stage="${1:?stage required}"
  local path="$S5B_RESULTS/$stage"
  [[ -d "$path" ]] || s5b_die "no results for $stage at $path"
  printf '%s\n' "$path"
}

check_sizes() {
  local path="$1" big
  big="$(find "$path" -type f -size +"${MAX_FILE_BYTES}"c -printf '%s %p\n')"
  [[ -z "$big" ]] || s5b_die "files above $((MAX_FILE_BYTES / 1024 / 1024)) MiB (keep results small):"$'\n'"$big"
  if find "$path" -type f \( -name '*.log' -o -name '*.log.gz' -o -name 'sirius_*.log*' \) | grep -q .; then
    s5b_die "raw log files under $path; results keep only excerpts"
  fi
}

check_branch() {
  local branch
  branch="$(git -C "$S5B_CONTROL" rev-parse --abbrev-ref HEAD)"
  [[ "$branch" == "$S5B_EXPERIMENT_BRANCH" ]] ||
    s5b_die "control checkout is on '$branch', expected $S5B_EXPERIMENT_BRANCH"
}

case "${1:-}" in
  show)
    path="$(stage_path "${2:-}")"
    check_sizes "$path"
    git -C "$S5B_CONTROL" status --short --untracked-files=all -- "$path"
    find "$path" -type f -printf '%10s  %P\n' | sort -k2
    du -sh "$path"
    ;;
  commit)
    check_branch
    stage="${2:-}"
    path="$(stage_path "$stage")"
    check_sizes "$path"
    git -C "$S5B_CONTROL" add -- "$path"
    if git -C "$S5B_CONTROL" diff --cached --quiet -- "$path"; then
      s5b_die "nothing new to commit for $stage"
    fi
    git -C "$S5B_CONTROL" commit -m "chore(s5b-mgpu): add $stage results" -- "$path"
    git -C "$S5B_CONTROL" log -1 --stat | head -40
    ;;
  push)
    check_branch
    url="$(git -C "$S5B_CONTROL" remote get-url "$S5B_FORK_REMOTE")"
    if [[ "$S5B_FORK_REMOTE" == "$S5B_UPSTREAM_REMOTE" || "$url" == *sirius-db/sirius* ]]; then
      s5b_die "refusing to push to $S5B_FORK_REMOTE ($url): results go only to the user's fork"
    fi
    git -C "$S5B_CONTROL" push "$S5B_FORK_REMOTE" "HEAD:refs/heads/$S5B_EXPERIMENT_BRANCH"
    ;;
  *) s5b_die "usage: publish_results.sh show <stage> | commit <stage> | push" ;;
esac
