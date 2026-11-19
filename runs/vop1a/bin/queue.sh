#!/bin/bash
# Usage: queue.sh <jobs file> [parallel waiters]
# Each line of the jobs file: "<ENV=VAL ...>|<label> <q> <iterations> <arm> ..." ; one lock hold per line.
jobs=$1; par=${2:-3}
R=/opt/dlami/nvme/dfperf; B=$R/runs/vop1a/bin; L=$R/machine.lock
run_line() {
  line=$1
  envs=${line%%|*}; args=${line#*|}
  # shellcheck disable=SC2086
  env $envs flock $L $B/pair.sh $args
}
export -f run_line; export R B L
grep -v '^#' "$jobs" | grep -v '^$' | xargs -P "$par" -d '\n' -I{} bash -c 'run_line "$1"' _ {}
echo "queue $jobs done $(date +%T)"
