#!/bin/bash
# Usage: unit.sh <tag> <iters> <q> <variant> [<variant> ...]   -- run as ONE locked command:
#   flock /opt/dlami/nvme/dfperf/machine.lock unit.sh b1 6 8 tip sh sh tip
# Variants: tip = $R/repo; off = wt-op2 no switches; sh = SHADOW=1; la = LATCH=1; shl = SHADOW=1 LATCH=1;
#           sk9 = oracle SKIP=29:1; ep = ENDPOINT=1.  Labels: <tag>_<variant>_<position>
tag=$1; iters=$2; q=$3; shift 3
R=/opt/dlami/nvme/dfperf; B=$R/runs/vop2/bin
pos=0
echo "unit tag=$tag q=$q begin $(date +%T) variants=$*" >> $R/runs/vop2/units.log
for v in "$@"; do
  pos=$((pos+1))
  case $v in
    tip) env REPO=$R/repo $B/one_query.sh ${tag}_${v}_$pos $q $iters ;;
    off) env REPO=$R/wt-op2 $B/one_query.sh ${tag}_${v}_$pos $q $iters ;;
    sh)  env REPO=$R/wt-op2 SIRIUS_PROTO_OP2_SHADOW=1 $B/one_query.sh ${tag}_${v}_$pos $q $iters ;;
    la)  env REPO=$R/wt-op2 SIRIUS_PROTO_OP2_LATCH=1 $B/one_query.sh ${tag}_${v}_$pos $q $iters ;;
    shl) env REPO=$R/wt-op2 SIRIUS_PROTO_OP2_SHADOW=1 SIRIUS_PROTO_OP2_LATCH=1 $B/one_query.sh ${tag}_${v}_$pos $q $iters ;;
    sk9) env REPO=$R/wt-op2 SIRIUS_PROTO_OP2_SKIP=29:1 $B/one_query.sh ${tag}_${v}_$pos $q $iters ;;
    ep)  env REPO=$R/wt-op2 SIRIUS_PROTO_OP2_ENDPOINT=1 $B/one_query.sh ${tag}_${v}_$pos $q $iters ;;
    tip2) env REPO=$R/repo SIRIUS_PRE_SQL="SET fused_scan_max_membership_sources = 2;" $B/one_query.sh ${tag}_${v}_$pos $q $iters ;;
    sh2) env REPO=$R/wt-op2 SIRIUS_PROTO_OP2_SHADOW=1 SIRIUS_PRE_SQL="SET fused_scan_max_membership_sources = 2;" $B/one_query.sh ${tag}_${v}_$pos $q $iters ;;
    *) echo "unknown variant $v" >&2 ;;
  esac
done
echo "unit tag=$tag q=$q end $(date +%T)" >> $R/runs/vop2/units.log
