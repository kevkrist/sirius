#!/bin/bash
# resdiff.sh q... : compare result.txt of every vsgb run (sgb, sgboff, tip, base arms) against the tip baseline result of the same query.
cd /opt/dlami/nvme/dfperf/runs/vsgb
for q in "$@"; do
  ref=$(ls /opt/dlami/nvme/dfperf/runs/baseline/baseline_tip/tpch_*_q$q/sirius/q$q/result.txt 2>/dev/null | head -1)
  [ -z "$ref" ] && { echo "q$q: no reference"; continue; }
  out="q$q:"
  for arm in sgb sgboff tip base; do
    n=0; bad=0
    for f in *_${arm}_[0-9]/tpch_*_q$q/sirius/q$q/result.txt lp_${arm}/tpch_*_q$q/sirius/q$q/result.txt zm_${arm}/tpch_*_q$q/sirius/q$q/result.txt; do
      [ -f "$f" ] || continue; n=$((n+1)); cmp -s $ref $f || { bad=$((bad+1)); echo "   DIFFERS: $f"; }
    done
    out="$out $arm runs=$n differing=$bad;"
  done
  echo "$out ref_lines=$(wc -l < $ref)"
done
