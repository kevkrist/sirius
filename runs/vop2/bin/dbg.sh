#!/bin/bash
# Usage: dbg.sh <label> <variant> <q> [<q> ...]  -- untimed DEBUG pass, 2 iterations; run as ONE locked command.
label=$1; v=$2; shift 2
R=/opt/dlami/nvme/dfperf; B=$R/runs/vop2/bin
export SIRIUS_LOG_LEVEL=${VLOG:-debug}
echo "dbg label=$label v=$v begin $(date +%T) q=$*" >> $R/runs/vop2/units.log
for q in "$@"; do
  case $v in
    tip) env REPO=$R/repo $B/one_query.sh $label $q 2 ;;
    off) env REPO=$R/wt-op2 $B/one_query.sh $label $q 2 ;;
    shl) env REPO=$R/wt-op2 SIRIUS_PROTO_OP2_SHADOW=1 SIRIUS_PROTO_OP2_LATCH=1 $B/one_query.sh $label $q 2 ;;
    tip2) env REPO=$R/repo SIRIUS_PRE_SQL="SET fused_scan_max_membership_sources = 2;" $B/one_query.sh $label $q 2 ;;
    off2) env REPO=$R/wt-op2 SIRIUS_PRE_SQL="SET fused_scan_max_membership_sources = 2;" $B/one_query.sh $label $q 2 ;;
    sh)  env REPO=$R/wt-op2 SIRIUS_PROTO_OP2_SHADOW=1 $B/one_query.sh $label $q 2 ;;
  esac
done
echo "dbg label=$label end $(date +%T)" >> $R/runs/vop2/units.log
