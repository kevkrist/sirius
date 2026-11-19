#!/bin/bash
# One hold: rebuild (num_gpus override), then q3 on GPUs 0,1 (PEER pair): sync (S1) vs async (B1), 3 iterations each.
R=/opt/dlami/nvme/dfperf; B=$R/runs/op1a/bin
$B/build.sh || exit 1
export CUDA_VISIBLE_DEVICES=0,1 SIRIUS_PROTO_NUM_GPUS=2
$B/pair.sh g2smoke 3 3 S1 B1
