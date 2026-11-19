#!/bin/bash
# One locked pass: build, smoke, main run, no-peer run, probe run, nsys capture.
R=/opt/dlami/nvme/dfperf; M=$R/runs/mb_xfer; B=$M/bin/xfer_bench
export TMPDIR=$R/tmp
cd $M
PHASES=${PHASES:-build,smoke,main,nopeer,probe,nsys}
has() { [[ ",$PHASES," == *",$1,"* ]] && [ ! -f out/.ok_$1 ]; }
ok() { touch out/.ok_$1; }
if has build; then
  $R/wt-mb-xfer/bench/transfer/build.sh > out/build.log 2>&1 || { echo "build failed"; grep -E 'error|Error' out/build.log | head -40; exit 10; }
  tail -1 out/build.log; ok build
fi
if has smoke; then
  timeout 600 $B tests=leg,pair,fan,hostor,conc,df,cont sizes=8 dfsizes=16 fansizes=1,8 bmax=64 reps=1 > out/smoke.log 2> out/smoke.err
  rc=$?; echo "smoke rc=$rc lines=$(wc -l < out/smoke.log)"
  [ $rc -ne 0 ] && { tail -5 out/smoke.err; tail -3 out/smoke.log; exit 11; }
  bad=$(grep -E '^R\|(fan|df|cont)\|' out/smoke.log | grep -v -E 'peerapi|ERR=' | grep 'mism=' | grep -c -v 'mism=0|')
  echo "smoke mismatching engineered schedules: $bad"
  [ "$bad" -ne 0 ] && { grep -E '^R\|(fan|df|cont)\|' out/smoke.log | grep -v -E 'peerapi|ERR=' | grep 'mism=' | grep -v 'mism=0|' | cut -c1-200 | head -20; exit 12; }
  ok smoke
fi
if has main; then
  /usr/bin/time -v -o out/main.time timeout 2400 $B reps=${REPS:-5} ${MAINARGS:-} > out/${MAINLOG:-main}.log 2> out/main.err; rc=$?; echo "main rc=$rc lines=$(wc -l < out/${MAINLOG:-main}.log)"; [ $rc -eq 0 ] && ok main
fi
if has nopeer; then
  timeout 900 $B nopeer=1 tests=pair pairmethods=peer reps=5 > out/nopeer.log 2> out/nopeer.err; rc=$?; echo "nopeer rc=$rc"; [ $rc -eq 0 ] && ok nopeer
fi
if has probe; then
  timeout 900 $B probe=1 tests=pair pairmethods=peer sizes=64,1434 reps=3 > out/probe.log 2> out/probe.err; rc=$?; echo "probe rc=$rc"; [ $rc -eq 0 ] && ok probe
fi
if has nsys; then
  mkdir -p nsys
  timeout 900 nsys profile --trace=cuda,nvtx --sample=none --cudabacktrace=none --cpuctxsw=none --force-overwrite=true --export=sqlite \
    -o $M/nsys/df281 $B tests=df dfsizes=281 reps=2 bmax=512 > out/nsys.log 2> out/nsys.err; rc=$?; echo "nsys rc=$rc"; [ $rc -eq 0 ] && ok nsys
fi
