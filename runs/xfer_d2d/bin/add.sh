#!/bin/bash
# add.sh <command line>: append one todo line
X=/opt/dlami/nvme/dfperf/runs/xfer_d2d
flock $X/todo.lock bash -c 'echo "$1" >> '$X/todo.txt -- "$*"
