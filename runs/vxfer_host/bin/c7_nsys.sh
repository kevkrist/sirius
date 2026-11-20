#!/bin/bash
# nsys: "<arms>" <q...>; one capture (1 warm + 2 hot) per arm per query (run under flock)
arms=$1; shift
for q in "$@"; do NSYS=1 /opt/dlami/nvme/dfperf/runs/vxfer_host/bin/pair.sh nsys $q 3 $arms; done
