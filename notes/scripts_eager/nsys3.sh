#!/bin/bash
# Usage: nsys3.sh <q> : nsys capture (3 iterations) of tip, option 1 and option 1 + scan throttle 1; one lock hold per process.
# Output runs/profile/eg_<arm>/, Quent telemetry/profile_eg_<arm>/q<q>/. nsys timings are inflated: never used for verdicts.
R=/opt/dlami/nvme/dfperf; N=$R/runs/profile/bin/nsys_query.sh; L=$R/machine.lock; q=$1
REPO=$R/repo flock $L $N eg_tip $q 3
REPO=$R/wt-eager2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 flock $L $N eg_o1 $q 3
REPO=$R/wt-eager2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_SCAN_THROTTLE=1 flock $L $N eg_k1 $q 3
echo "NSYS_DONE q$q $(date +%T)"
