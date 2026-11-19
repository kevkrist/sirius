#!/bin/bash
# G=4 sweep, then the G=2 variants. Run under flock.
/opt/dlami/nvme/dfperf/runs/xfer_host/bin/mb_full.sh
/opt/dlami/nvme/dfperf/runs/xfer_host/bin/mb_g2.sh
