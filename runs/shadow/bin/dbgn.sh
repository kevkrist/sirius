#!/bin/bash
# Usage: dbgn.sh <label> <tip|prod|tip2|prod2> <q> [<q> ...]  -- untimed DEBUG pass, 2 iterations per query, ONE lock hold.
# Run under: flock /opt/dlami/nvme/dfperf/machine.lock dbgn.sh ...   (keep to about 4 queries)
label=$1; v=$2; shift 2
R=/opt/dlami/nvme/dfperf; B=$R/runs/shadow/bin
export SIRIUS_LOG_LEVEL=debug
for q in "$@"; do
  case $v in
    tip)   REPO=$R/repo       $B/one_query.sh $label $q 2 ;;
    prod)  REPO=$R/wt-prod-70 $B/one_query.sh $label $q 2 ;;
    tip2)  REPO=$R/repo       SIRIUS_PRE_SQL="SET fused_scan_max_membership_sources = 2;" $B/one_query.sh $label $q 2 ;;
    prod2) REPO=$R/wt-prod-70 SIRIUS_PRE_SQL="SET fused_scan_max_membership_sources = 2;" $B/one_query.sh $label $q 2 ;;
  esac
done
