#!/bin/bash
# Usage: nsys_block.sh "<arm> ..." <q> [<q> ...]   -- run under flock: one nsys capture (1 warm + 2 hot) per arm per query
arms=$1; shift
for q in "$@"; do NSYS=1 /opt/dlami/nvme/dfperf/runs/xfer_host/bin/pair.sh nsys $q 3 $arms; done
