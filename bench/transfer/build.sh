#!/bin/bash
# Builds the standalone transfer microbenchmark with the toolchain of the reference pixi env
# (read-only use of $R/repo/.pixi). Run under: flock $R/machine.lock bench/transfer/build.sh
R=${R:-/opt/dlami/nvme/dfperf}
E=$R/repo/.pixi/envs/default
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$R/runs/mb_xfer/bin}
export PATH=$E/bin:$PATH TMPDIR=$R/tmp
mkdir -p "$OUT" "$TMPDIR"
CXX=$E/bin/x86_64-conda-linux-gnu-c++
CUDA_INC=$E/targets/x86_64-linux/include
DEFS="-DCCCL_DISABLE_PDL -DCCCL_IGNORE_DEPRECATED_STREAM_REF_HEADER -DLIBCUDACXX_ENABLE_EXPERIMENTAL_MEMORY_RESOURCE -DFMT_SHARED -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DSPDLOG_SHARED_LIB"
INC="-I$E/include/rapids -isystem $E/include -I$CUDA_INC -isystem $CUDA_INC/cccl"
set -x
$E/bin/nvcc -ccbin $CXX -O3 -std=c++20 \
  "--generate-code=arch=compute_90a,code=[sm_90a]" "--generate-code=arch=compute_100f,code=[sm_100f]" \
  "--generate-code=arch=compute_120f,code=[sm_120f]" \
  -I$CUDA_INC -c $SRC/xfer_kernels.cu -o $OUT/xfer_kernels.o || exit 1
build_main() {
  $CXX $1 $DEFS $INC -O3 -DNDEBUG -std=gnu++20 -fopenmp -mavx512f -mavx512bw -Wall -Wno-unknown-pragmas \
    -c $SRC/xfer_bench.cpp -o $OUT/xfer_bench.o &&
  $CXX $OUT/xfer_bench.o $OUT/xfer_kernels.o -o $OUT/xfer_bench -fopenmp \
    -L$E/lib -L$E/targets/x86_64-linux/lib $2 -lcudart -ldl -lpthread \
    -Wl,-rpath,$E/lib -Wl,-rpath,$E/targets/x86_64-linux/lib
}
if build_main -DUSE_RMM "-lrmm -lrapids_logger"; then
  echo "BUILD_OK rmm=1"
elif build_main "" ""; then
  echo "BUILD_OK rmm=0 (RMM build failed; pool created with the same properties through raw CUDA)"
else
  echo "BUILD_FAILED"; exit 1
fi
