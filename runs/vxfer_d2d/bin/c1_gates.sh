#!/bin/bash
# author's own gates re-run in a fresh process (prototype binary): Bloom bit identity G=4, relay identity
B=/opt/dlami/nvme/dfperf/runs/vxfer_d2d/bin
UT_TIMEOUT=400 $B/ut_run.sh gate_mbid_g4 0,1,2,3 "[mbid]" SIRIUS_BENCH_DF_GPUS=4
UT_TIMEOUT=200 $B/ut_run.sh gate_relay 0,1,2,3 "[xfer_relay]"
