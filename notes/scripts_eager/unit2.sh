#!/bin/bash
# Usage (under the machine lock): unit2.sh <tag> <iterations> <q> <arm:n> [<arm:n> ...]
# Runs the listed processes back to back, fresh process each; label = <tag>_<arm><n>.
# Arms (E2 = wt-eager2, option 1 = publication inside PARTITION, then activation at PARTITION finalize):
#   tip   R/repo                           pro   wt-eager, eager 1 (recovered option 1 commit)
#   o1    E2 eager 1, gate 0               g1/g2 E2 eager 1, gate 1 (S6 literal) / 2 (started scans terminal)
#   s2    E2 eager 2, gate 0               s2g1/s2g2  eager 2 with gate 1 / 2
#   k1/k2/k3  E2 eager 1 + scan-only throttle K      ka1/ka2  first-version throttle (all tasks)
#   o2    E2 option 2 (deferred publication + callback)     adm  eager 1, gate 2, admit 1000 GiB (never admitted)
#   bs    o1 + CUCASCADE_PROTO_BATCH_STAGING=1 (build move queues the whole table's copies at once)   b0  same, eager off
#   bsk   bs + scan throttle 1          bf   f1 + batched staging
#   f1    E2 eager 2, gate 2, admit 16 GiB (S6 shape)   f1k  f1 + scan throttle 2
tag=$1; iters=$2; q=$3; shift 3
B=/opt/dlami/nvme/dfperf/runs/eager/bin; R=/opt/dlami/nvme/dfperf; E2=$R/wt-eager2
for spec in "$@"; do
  arm=${spec%%:*}; n=${spec##*:}; label=${tag}_${arm}${n}
  case $arm in
    tip)  REPO=$R/repo $B/one_query.sh $label $q $iters ;;
    pro)  REPO=$R/wt-eager SIRIUS_EAGER_PROBE=1 $B/one_query.sh $label $q $iters ;;
    o1)   REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 $B/one_query.sh $label $q $iters ;;
    g1)   REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_GATE=1 $B/one_query.sh $label $q $iters ;;
    g2)   REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_GATE=2 $B/one_query.sh $label $q $iters ;;
    s2)   REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 $B/one_query.sh $label $q $iters ;;
    s2g1) REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=1 $B/one_query.sh $label $q $iters ;;
    s2g2) REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=2 $B/one_query.sh $label $q $iters ;;
    k1|k2|k3) REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_SCAN_THROTTLE=${arm#k} $B/one_query.sh $label $q $iters ;;
    ka1|ka2) REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_THROTTLE_ALL=1 SIRIUS_EAGER_SCAN_THROTTLE=${arm#ka} $B/one_query.sh $label $q $iters ;;
    o2)   REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=1 SIRIUS_EAGER_PROBE=1 $B/one_query.sh $label $q $iters ;;
    adm)  REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_GATE=2 SIRIUS_EAGER_ADMIT_GIB=1000 $B/one_query.sh $label $q $iters ;;
    f1)   REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=2 SIRIUS_EAGER_ADMIT_GIB=16 $B/one_query.sh $label $q $iters ;;
    f1k)  REPO=$E2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=2 SIRIUS_EAGER_ADMIT_GIB=16 SIRIUS_EAGER_SCAN_THROTTLE=2 $B/one_query.sh $label $q $iters ;;
    bs)   REPO=$E2 CUCASCADE_PROTO_BATCH_STAGING=1 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 $B/one_query.sh $label $q $iters ;;
    b0)   REPO=$E2 CUCASCADE_PROTO_BATCH_STAGING=1 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=0 $B/one_query.sh $label $q $iters ;;
    bsk)  REPO=$E2 CUCASCADE_PROTO_BATCH_STAGING=1 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_SCAN_THROTTLE=1 $B/one_query.sh $label $q $iters ;;
    bf)   REPO=$E2 CUCASCADE_PROTO_BATCH_STAGING=1 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=2 SIRIUS_EAGER_ADMIT_GIB=16 $B/one_query.sh $label $q $iters ;;
    *) echo "unknown arm $arm"; exit 9 ;;
  esac
done
