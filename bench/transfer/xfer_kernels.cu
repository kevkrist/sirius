// Kernels for the transfer-primitive microbenchmark (prototype tooling, not engine code).
#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace {

__device__ __forceinline__ uint64_t mix(uint64_t x)
{
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// Word i of the "partial" owned by every device in `mask`, OR-ed together.
__device__ __forceinline__ uint64_t pattern(uint64_t i, unsigned mask, uint64_t salt)
{
  uint64_t v = 0;
  for (unsigned g = 0; g < 4; ++g) {
    if ((mask >> g) & 1u) {
      uint64_t a = mix(i * 4 + g + salt * 0x100000001B3ull);
      v |= a & mix(a ^ 0x5555555555555555ull);
    }
  }
  return v;
}

__global__ void k_fill(uint64_t* p, size_t n, unsigned mask, uint64_t salt)
{
  size_t const stride = static_cast<size_t>(gridDim.x) * blockDim.x;
  for (size_t i = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n; i += stride) {
    p[i] = pattern(i, mask, salt);
  }
}

__global__ void k_verify(uint64_t const* p,
                         size_t n,
                         unsigned mask,
                         uint64_t salt,
                         unsigned long long* mismatches)
{
  size_t const stride = static_cast<size_t>(gridDim.x) * blockDim.x;
  unsigned long long bad = 0;
  for (size_t i = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n; i += stride) {
    bad += (p[i] != pattern(i, mask, salt)) ? 1ull : 0ull;
  }
  if (bad != 0) { atomicAdd(mismatches, bad); }
}

// Same shape as the engine's or_bloom_chunk: 16-byte loads/stores, 256 threads, <= 4096 blocks.
__global__ void k_or(uint4* dst, uint4 const* src, size_t n)
{
  size_t const stride = static_cast<size_t>(gridDim.x) * blockDim.x;
  for (size_t i = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n; i += stride) {
    uint4 a       = dst[i];
    uint4 const b = src[i];
    a.x |= b.x;
    a.y |= b.y;
    a.z |= b.z;
    a.w |= b.w;
    dst[i] = a;
  }
}

unsigned grid_for(size_t n)
{
  size_t const blocks = (n + 255) / 256;
  return static_cast<unsigned>(blocks < 1 ? 1 : (blocks > 4096 ? 4096 : blocks));
}

}  // namespace

// All launchers run on the caller's current device.
cudaError_t launch_fill(void* p, size_t bytes, unsigned mask, uint64_t salt, cudaStream_t stream)
{
  size_t const n = bytes / 8;
  if (n == 0) { return cudaSuccess; }
  k_fill<<<grid_for(n), 256, 0, stream>>>(static_cast<uint64_t*>(p), n, mask, salt);
  return cudaGetLastError();
}

cudaError_t launch_verify(void const* p,
                          size_t bytes,
                          unsigned mask,
                          uint64_t salt,
                          unsigned long long* mismatches,
                          cudaStream_t stream)
{
  size_t const n = bytes / 8;
  if (n == 0) { return cudaSuccess; }
  k_verify<<<grid_for(n), 256, 0, stream>>>(
    static_cast<uint64_t const*>(p), n, mask, salt, mismatches);
  return cudaGetLastError();
}

cudaError_t launch_or(void* dst, void const* src, size_t bytes, cudaStream_t stream)
{
  size_t const n = bytes / 16;
  if (n == 0) { return cudaSuccess; }
  k_or<<<grid_for(n), 256, 0, stream>>>(static_cast<uint4*>(dst), static_cast<uint4 const*>(src), n);
  return cudaGetLastError();
}
