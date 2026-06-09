#!/usr/bin/env bash
# For each probe query: run cpu / eager / latemat, diff the (sorted, deterministic)
# CSV outputs against the CPU ground truth, and report whether late-mat actually fired.
set -uo pipefail
cd "$(dirname "$0")"
D=$(pwd)
mkdir -p out logs

probes=("$@")
if [ ${#probes[@]} -eq 0 ]; then
  probes=(queries/*.sql)
fi

printf "%-28s %8s %10s %11s %11s %14s %8s\n" "probe" "rows" "late_mat?" "cpu=eager" "cpu=latmat" "eager=latmat" "verdict"
for q in "${probes[@]}"; do
  name=$(basename "$q" .sql)
  for cfg in cpu eager latemat; do
    ./run_query.sh "$cfg" "$D/out/${name}_${cfg}.csv" "$D/logs/${name}_${cfg}" < "$q" 2>"$D/out/${name}_${cfg}.err"
  done
  rows=$(($(wc -l < "$D/out/${name}_cpu.csv") - 1))
  fired=$(grep -rl "late-materializing varchar columns" "$D/logs/${name}_latemat" >/dev/null 2>&1 && echo yes || echo NO)
  # Catch any error surfaced to stderr (engine exceptions etc.)
  err=""
  for cfg in cpu eager latemat; do
    if grep -qiE "Error|Exception" "$D/out/${name}_${cfg}.err"; then err="${err}${cfg} "; fi
  done
  if diff -q "$D/out/${name}_cpu.csv" "$D/out/${name}_eager.csv"   >/dev/null 2>&1; then ce=MATCH; else ce=DIFF; fi
  if diff -q "$D/out/${name}_cpu.csv" "$D/out/${name}_latemat.csv" >/dev/null 2>&1; then cl=MATCH; else cl=DIFF; fi
  if diff -q "$D/out/${name}_eager.csv" "$D/out/${name}_latemat.csv" >/dev/null 2>&1; then el=MATCH; else el=DIFF; fi
  # eager==latemat is the definitive late-mat correctness signal (identical GPU
  # float behavior). cpu diffs on aggregate queries can be low-order float noise.
  verdict=PASS
  [ "$el" = DIFF ] && verdict=FAIL
  [ -n "$err" ] && verdict="ERR:$err"
  printf "%-28s %8s %10s %11s %11s %14s %8s\n" "$name" "$rows" "$fired" "$ce" "$cl" "$el" "$verdict"
done
