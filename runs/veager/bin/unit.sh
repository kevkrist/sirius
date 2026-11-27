#!/bin/bash
# Usage (under the machine lock): unit.sh <tag> <iters> <q[,q]> <arm:n> ...   verifier's arms; binaries read-only.
#   tip = $R/repo ; f1 = wt-eager2 eager 2 + gate 2 + admit 16 (S6 shape) ; fk1 = f1 + throttle 1 ; o1 = eager 1 gate 0
#   g1 = eager 1 gate 1 ; s2g1 = eager 2 gate 1 ; off = wt-eager2 with eager off (binary control)
tag=$1; iters=$2; qlist=$3; shift 3
R=/opt/dlami/nvme/dfperf; B=$R/runs/veager/bin; E2=$R/wt-eager2
for q in ${qlist//,/ }; do
for spec in "$@"; do
  arm=${spec%%:*}; n=${spec##*:}; label=${tag}_${arm}${n}
  case $arm in
    tip) REPO=$R/repo $B/one_query.sh $label $q $iters ;;
    off) REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=0 $B/one_query.sh $label $q $iters ;;
    o1)  REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 $B/one_query.sh $label $q $iters ;;
    g1)  REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_GATE=1 $B/one_query.sh $label $q $iters ;;
    f1)  REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=2 SIRIUS_EAGER_ADMIT_GIB=16 $B/one_query.sh $label $q $iters ;;
    fk1) REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=2 SIRIUS_EAGER_ADMIT_GIB=16 SIRIUS_EAGER_SCAN_THROTTLE=1 $B/one_query.sh $label $q $iters ;;
    *) echo "unknown arm $arm"; exit 9 ;;
  esac
done
done
