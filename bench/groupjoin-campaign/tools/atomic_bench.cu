// Scattered-atomic microbenchmark for groupjoin-framework-design.md §4.4 (PR-3 prerequisite).
// Models the INNER-form counted pass at SF1000 q17 scale: N rows, each doing
// atomicAdd(matched[k], 1) (u64) + atomicAdd(sum[k], v) (i64) at a random slot in a
// DOMAIN-sized state region (AVG bundle: presence u32 + matched u64 + sum i64 = 20 B/slot).
// Keys are generated in-register (SplitMix64) so no key traffic competes with the atomics.
#include <cstdio>
#include <cstdint>
#include <cuda_runtime.h>

#define CHECK(x) do { cudaError_t e = (x); if (e != cudaSuccess) { \
  printf("CUDA error %s at line %d\n", cudaGetErrorString(e), __LINE__); return 1; } } while (0)

__device__ __forceinline__ uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

__global__ void scattered_accumulate(unsigned long long* matched, long long* sum,
                                     uint64_t domain, uint64_t n_rows) {
  uint64_t stride = (uint64_t)gridDim.x * blockDim.x;
  for (uint64_t i = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x; i < n_rows; i += stride) {
    uint64_t h = splitmix64(i);
    uint64_t k = h % domain;
    atomicAdd(&matched[k], 1ULL);
    atomicAdd(reinterpret_cast<unsigned long long*>(&sum[k]), (unsigned long long)(h >> 32));
  }
}

int main(int argc, char** argv) {
  uint64_t domain = 200'000'000ULL;               // p_partkey domain at SF1000
  uint64_t n_rows = argc > 1 ? strtoull(argv[1], nullptr, 10) : 6'000'000'000ULL;
  unsigned long long* matched; long long* sum;
  CHECK(cudaMalloc(&matched, domain * sizeof(unsigned long long)));
  CHECK(cudaMalloc(&sum, domain * sizeof(long long)));
  CHECK(cudaMemset(matched, 0, domain * 8));
  CHECK(cudaMemset(sum, 0, domain * 8));
  int dev; cudaDeviceProp p; CHECK(cudaGetDevice(&dev)); CHECK(cudaGetDeviceProperties(&p, dev));
  dim3 grid(p.multiProcessorCount * 8), block(256);
  // warmup
  scattered_accumulate<<<grid, block>>>(matched, sum, domain, 100'000'000ULL);
  CHECK(cudaDeviceSynchronize());
  cudaEvent_t t0, t1; cudaEventCreate(&t0); cudaEventCreate(&t1);
  cudaEventRecord(t0);
  scattered_accumulate<<<grid, block>>>(matched, sum, domain, n_rows);
  cudaEventRecord(t1);
  CHECK(cudaEventSynchronize(t1));
  float ms; cudaEventElapsedTime(&ms, t0, t1);
  double atomics = 2.0 * (double)n_rows;
  printf("%s | rows=%llu domain=%llu state=%.1f GB | %.1f ms | %.2f G atomics/s | %.1f GB effective RMW traffic\n",
         p.name, (unsigned long long)n_rows, (unsigned long long)domain,
         domain * 16.0 / 1e9, ms, atomics / (ms * 1e6), atomics * 16.0 / 1e9);
  return 0;
}
