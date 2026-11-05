#!/usr/bin/env bash
# Run a command while holding the campaign GPU lock. Never exec (EXIT trap must fire).
LOCK=/home/ubuntu/sirius-scaling-20261105/gpu.lock
until mkdir "$LOCK" 2>/dev/null; do sleep 10; done
echo "$$ $(date -Is) $*" > "$LOCK/owner"
trap 'rm -rf "$LOCK"' EXIT
"$@"
