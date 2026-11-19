#!/bin/bash
# Usage: submods.sh <worktree>   Initialise the submodules the build needs from the local clones in $R/repo/.git/modules
# (no network, no experimental/* engines). Not a build, no GPU: runs without the machine lock.
set -e
R=/opt/dlami/nvme/dfperf; W=$1; M=$R/repo/.git/modules
cd $W
for s in cucascade duckdb substrait sirius-duckdb/duckdb sirius-duckdb/extension-ci-tools sirius-duckdb/vcpkg .claude/claude-tools; do
  git submodule init -- $s
  [ -d "$M/$s" ] && git config submodule.$s.url $M/$s
  git -c protocol.file.allow=always submodule update -- $s
done
cd $W/substrait
for s in duckdb substrait extension-ci-tools; do
  git submodule init -- $s
  [ -d "$M/substrait/modules/$s" ] && git config submodule.$s.url $M/substrait/modules/$s
  git -c protocol.file.allow=always submodule update -- $s || echo "WARN nested $s failed"
done
cd $W; git submodule status | cut -c1-90
