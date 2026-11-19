#!/bin/bash
# unit.sh <mode> <label> <catch filter>: existing unit tests with the switch at <mode> (off = tip path), 4 GPUs visible.
B=/opt/dlami/nvme/dfperf/runs/xfer_d2d/bin
timeout 900 $B/mb.sh unit_$2_$1 0,1,2,3 "$3" SIRIUS_PROTO_XFER_D2D=$1
