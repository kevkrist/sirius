#!/bin/bash
# Usage: mutant.sh <M1|M2|clean>  -- oracle check of the randomized Consumer agreement test, in wt-prod-69.
# M1: a tie in decode preference counts for the published filter. M2: taken decode slots alone make a filter unselected.
# clean: restores the source, rebuilds, reruns. Run under the machine lock. Source is always restored before exit.
R=/opt/dlami/nvme/dfperf; X=$R/runs/shadow_fix; wt=wt-prod-69; f=src/op/dynamic_filter/static_filter_selection.cpp
cd $R/$wt || exit 9
git checkout -- $f
case $1 in
  M1) sed -i 's|return decode_preference(other, candidate) < 0;|return decode_preference(other, candidate) <= 0;|' $f ;;
  M2) sed -i 's|  return !negligible_after_stronger(|  return false \&\& !negligible_after_stronger(|' $f ;;
esac
changed=$(git diff --numstat -- $f | cut -f1)
$R/runs/shadow/bin/build.sh $wt; brc=$?
$X/bin/shadow_test.sh $wt; trc=$?
cp $X/unit/$wt/shadow_s.txt $X/unit/$wt/shadow_$1.txt
git checkout -- $f
echo "mutant=$1 lines_changed=${changed:-0} build_rc=$brc test_rc=$trc $(date +%T) $(grep -c 'FAILED:' $X/unit/$wt/shadow_$1.txt) failed assertions; $(tail -3 $X/unit/$wt/shadow_$1.txt | tr '\n' ' ')" >> $X/mutants.txt
