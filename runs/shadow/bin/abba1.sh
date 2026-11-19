#!/bin/bash
# Usage: abba1.sh <tag> <iters> <q>  -- one query, 4 fresh processes (tip A, prod A, prod B, tip B) in ONE lock hold.
# Run under: flock /opt/dlami/nvme/dfperf/machine.lock abba1.sh ...   (about 3-4 minutes)
tag=$1; iters=$2; q=$3
R=/opt/dlami/nvme/dfperf; B=$R/runs/shadow/bin
REPO=$R/repo       $B/one_query.sh ${tag}_tipA $q $iters
REPO=$R/wt-prod-70 $B/one_query.sh ${tag}_prodA $q $iters
REPO=$R/wt-prod-70 $B/one_query.sh ${tag}_prodB $q $iters
REPO=$R/repo       $B/one_query.sh ${tag}_tipB $q $iters
