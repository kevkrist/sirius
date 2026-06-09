#!/usr/bin/env bash
# Sweep the 22 canonical TPC-H queries under the late-mat config and report,
# per query: did it run, and how many scan splits actually late-materialized
# (late_mat=true) vs fell back to eager (late_mat=false).
set -uo pipefail
cd "$(dirname "$0")"
D=$(pwd)
QDIR=../tpch_queries/orig

printf "%-5s %-7s %10s %12s %8s\n" "query" "status" "LM_split" "eager_split" "rows"
for n in $(seq 1 22); do
  q="$QDIR/q$n.sql"
  [ -f "$q" ] || continue
  timeout 240 ./run_query.sh latemat "$D/out/tpch_q${n}_latemat.csv" "$D/logs/tpch_q${n}_latemat" \
    < "$q" 2>"$D/out/tpch_q${n}.err"
  rc=$?
  status=OK
  [ $rc -eq 124 ] && status=TIMEOUT
  grep -qiE "Error|Exception" "$D/out/tpch_q${n}.err" 2>/dev/null && status=ERROR
  lmt=$(grep -roh "late_mat=true"  "$D/logs/tpch_q${n}_latemat/" 2>/dev/null | wc -l)
  lmf=$(grep -roh "late_mat=false" "$D/logs/tpch_q${n}_latemat/" 2>/dev/null | wc -l)
  rows=$(( $(wc -l < "$D/out/tpch_q${n}_latemat.csv" 2>/dev/null || echo 1) - 1 ))
  printf "%-5s %-7s %10s %12s %8s\n" "q$n" "$status" "$lmt" "$lmf" "$rows"
done
