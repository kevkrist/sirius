#!/bin/bash
# nsys_q.sh <q>: nsys capture of one query, tip and prototype, 3 iterations each (iteration 0 warm).
X=/opt/dlami/nvme/dfperf/runs/xfer_d2d; R=/opt/dlami/nvme/dfperf
[ -e $X/GATE_OK ] || { echo "nsys: gate not passed"; exit 1; }
env -u SIRIUS_PROTO_XFER_D2D NSYS=1 REPO=$R/repo $X/bin/one_query.sh ns_tip $1 3
SIRIUS_PROTO_XFER_D2D=exchange NSYS=1 REPO=$R/wt-xfer-d2d $X/bin/one_query.sh ns_proto $1 3
