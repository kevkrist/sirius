#!/bin/bash
# Follow-up pass: rebuild, then leg-size robustness under background copies (tests=cont2).
R=/opt/dlami/nvme/dfperf; M=$R/runs/mb_xfer; B=$M/bin/xfer_bench
export TMPDIR=$R/tmp
cd $M
[ -f out/.ok_cont2 ] && exit 0
$R/wt-mb-xfer/bench/transfer/build.sh > out/build2.log 2>&1 || { echo "build failed"; grep -E 'error' out/build2.log | head -30; exit 10; }
tail -1 out/build2.log
timeout 1500 $B tests=cont2 dfsizes=281,1434 reps=3 > out/cont2.log 2> out/cont2.err; rc=$?
echo "cont2 rc=$rc lines=$(wc -l < out/cont2.log)"; [ $rc -eq 0 ] && touch out/.ok_cont2
