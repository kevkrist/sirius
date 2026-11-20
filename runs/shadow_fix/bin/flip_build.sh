#!/bin/bash
# Builds wt-prod-70 with the measurement-only patch flip/late_only.patch applied (env SIRIUS_DF_SHADOW_LATE_ONLY skips
# the judgement at `begin`). Keeps the previous library as flip/libsirius.prod.so. Run under the machine lock.
R=/opt/dlami/nvme/dfperf; F=$R/runs/shadow_fix/flip
lib=$R/wt-prod-70/build/release/install/lib/libsirius.so.0.0.0
[ -f $F/libsirius.prod.so ] || cp -p $lib $F/libsirius.prod.so
$R/runs/shadow/bin/build.sh wt-prod-70; rc=$?
cp -p $R/logs/build_wt-prod-70.log $F/build.log
[ $rc = 0 ] && cp -p $lib $F/libsirius.flip.so
echo "flip_build rc=$rc $(date +%T) $(md5sum $lib | cut -c1-12)" >> $F/status.txt
exit $rc
