#!/bin/bash
# (1) mixed-kind fallback check: zone maps on -> construction is not Bloom-only -> prototype must take the tip path. q3, debug log, 2 iterations.
# (2) controls: q6 (no DF), q11 (IN-list only) tip/sgb ABBA.
B=/opt/dlami/nvme/dfperf/runs/vsgb/bin
export SIRIUS_ENABLE_TEST_OPTIONS=1 SIRIUS_PRE_SQL="SET enable_dynamic_zone_map_filter = true;" SIRIUS_LOG_LEVEL=debug
$B/oneq.sh tip zm_tip 3 2; echo "zm tip rc=$? $(date +%T)"
$B/oneq.sh sgb zm_sgb 3 2; echo "zm sgb rc=$? $(date +%T)"
unset SIRIUS_ENABLE_TEST_OPTIONS SIRIUS_PRE_SQL SIRIUS_LOG_LEVEL
$B/arms.sh c1 6 6 tip sgb sgb tip
$B/arms.sh c1 6 11 sgb tip tip sgb
# (3) q7 control with the env-off arm of the prototype binary: separates the mode from the binary.
$B/arms.sh c2 6 7 tip sgboff sgb sgb sgboff tip
