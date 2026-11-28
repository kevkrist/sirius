#!/bin/bash
# gate.sh nondf|logpass : nondf = the 5 non-DF queries, every arm, 2 iterations (result identity, labels g_<arm>);
# logpass = DEBUG log, 3 iterations, every arm, queries with one-shot Bloom sites (labels lp_<arm>). Lock per query.
R=/opt/dlami/nvme/dfperf; B=$R/runs/hybridfull/bin; ARMS="T S X HX HXF EX EHX EHXF"
case $1 in
  nondf) for q in 1 6 13 14 15; do flock $R/machine.lock bash -c "for a in $ARMS; do $B/arm.sh \$a g_\$a $q 2; done"; done;;
  logpass) shift; for q in "$@"; do flock $R/machine.lock bash -c "for a in $ARMS; do SIRIUS_LOG_LEVEL=debug $B/arm.sh \$a lp_\$a $q 3; done"; done;;
esac
echo GATEDONE $1
