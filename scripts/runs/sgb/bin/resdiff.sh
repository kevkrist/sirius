#!/bin/bash
# resdiff.sh : compare sirius/q<N>/result.txt of every sgb run against a tip run of the same query (all blocks)
cd /opt/dlami/nvme/dfperf/runs/sgb
for q in "$@"; do
  ref=$(ls t1_tip_*/tpch_*_q$q/sirius/q$q/result.txt 2>/dev/null | head -1)
  [ -z "$ref" ] && ref=$(ls /opt/dlami/nvme/dfperf/runs/baseline/baseline_tip/tpch_*_q$q/sirius/q$q/result.txt | head -1)
  n=0; bad=0
  for f in *_sgb_*/tpch_*_q$q/sirius/q$q/result.txt; do [ -f "$f" ] || continue; n=$((n+1)); cmp -s $ref $f || bad=$((bad+1)); done
  echo "q$q: sgb runs=$n differing=$bad ref_lines=$(wc -l < $ref) ref=$(dirname $ref | cut -c1-40)"
done
