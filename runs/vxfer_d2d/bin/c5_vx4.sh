#!/bin/bash
# build the second verifier binary (adds VX4) and run VX4
B=/opt/dlami/nvme/dfperf/runs/vxfer_d2d/bin
$B/vx_build2.sh
UTBIN=/opt/dlami/nvme/dfperf/runs/vxfer_d2d/vxbin2/sirius_unittest_vx UT_TIMEOUT=200 $B/ut_run.sh vx4 0,1,2,3 "[vx4]"
