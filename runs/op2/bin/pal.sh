#!/bin/bash
# Usage: pal.sh <tag> <iters> "<q> [<q>...]" <variant> [<variant> ...]
# variant = name[:ENV=val[;ENV=val...]] ; name "tip" = $R/repo binary, anything else = $R/wt-op2 with the env.
# Per query runs the variants in palindromic order (A B C C B A), each a fresh locked process.
# Labels: <tag>_<name>_A (first pass) / <tag>_<name>_B (second pass).
tag=$1; iters=$2; qs=$3; shift 3
B=/opt/dlami/nvme/dfperf/runs/op2/bin; L=/opt/dlami/nvme/dfperf/machine.lock; R=/opt/dlami/nvme/dfperf
run() { # spec pass q
  local name=${1%%:*} envs=""; [[ "$1" == *:* ]] && envs=$(echo "${1#*:}" | tr ';' ' ')
  local repo=$R/wt-op2; [ "$name" = tip ] && repo=$R/repo
  env $envs REPO=$repo flock $L $B/one_query.sh ${tag}_${name}_$2 $3 $iters
}
for q in $qs; do
  for v in "$@"; do run "$v" A $q; done
  for ((i=$#; i>=1; i--)); do run "${!i}" B $q; done
done
