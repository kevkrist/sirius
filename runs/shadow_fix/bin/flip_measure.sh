#!/bin/bash
# Usage: flip_measure.sh <tag> <iters> <q>   -- forced order flip, one lock hold (about 5 minutes).
# Arms: tip (R/repo), late (wt-prod-70 flip build, SIRIUS_DF_SHADOW_LATE_ONLY=1: judged only before finish),
# begin (same build, env unset: judged at begin = prod behaviour). Order tip, late, begin, begin, late, tip.
# First a 2-iteration DEBUG process of the late arm to prove `shadowed_late` fires.
tag=$1; iters=$2; q=$3
R=/opt/dlami/nvme/dfperf; B=$R/runs/shadow/bin
lib=$R/wt-prod-70/build/release/install/lib/libsirius.so.0.0.0
cmp -s $lib $R/runs/shadow_fix/flip/libsirius.flip.so || { echo "flip library not installed" >&2; exit 3; }
SIRIUS_DF_SHADOW_LATE_ONLY=1 SIRIUS_LOG_LEVEL=debug REPO=$R/wt-prod-70 $B/one_query.sh ${tag}_dbg_late $q 2
REPO=$R/repo       $B/one_query.sh ${tag}_tipA $q $iters
SIRIUS_DF_SHADOW_LATE_ONLY=1 REPO=$R/wt-prod-70 $B/one_query.sh ${tag}_lateA $q $iters
REPO=$R/wt-prod-70 $B/one_query.sh ${tag}_beginA $q $iters
REPO=$R/wt-prod-70 $B/one_query.sh ${tag}_beginB $q $iters
SIRIUS_DF_SHADOW_LATE_ONLY=1 REPO=$R/wt-prod-70 $B/one_query.sh ${tag}_lateB $q $iters
REPO=$R/repo       $B/one_query.sh ${tag}_tipB $q $iters
