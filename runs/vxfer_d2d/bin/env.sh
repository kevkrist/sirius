export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export X=$R/runs/vxfer_d2d W=$R/wt-xfer-d2d
export UT=build/release/test/cpp/sirius_unittest
mkdir -p $TMPDIR $X/out
