/*
 * Copyright 2026, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "exec/invocable.hpp"
#include "io/cache/prefetching_cache.hpp"
#include "io/deferred_device_batch.hpp"
#include "io/io_context.hpp"
#include "io/types.hpp"

#include <cudf/io/datasource.hpp>
#include <cudf/io/text/byte_range_info.hpp>
#include <cudf/version_config.hpp>

#if CUDF_VERSION_MAJOR < 26 || (CUDF_VERSION_MAJOR == 26 && CUDF_VERSION_MINOR < 10)
#include <rmm/cuda_stream_view.hpp>
#endif

#include <cuda/stream>
#include <cuda_runtime_api.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace sirius::io {

// cudf >= 26.10 (rapidsai/cudf#23669) takes cuda::stream_ref in the
// device_read virtuals; overrides must match the base signature exactly.
#if CUDF_VERSION_MAJOR > 26 || (CUDF_VERSION_MAJOR == 26 && CUDF_VERSION_MINOR >= 10)
using cudf_datasource_stream_t = ::cuda::stream_ref;
#else
using cudf_datasource_stream_t = rmm::cuda_stream_view;
#endif

// ---------------------------------------------------------------------------
// sirius_datasource
// ---------------------------------------------------------------------------

/**
 * Why a datasource did or did not start a prefetch, so the readahead can attribute a refusal
 * instead of only counting one.  A refusal is normal -- the readahead offers work the read path is
 * free to turn down -- but the reason decides whether it means "we are out of memory" or "we are
 * too late".
 */
enum class prefetch_refusal : std::uint8_t {
  /**
   * IO went out.
   */
  issued,
  /**
   * No prefetching cache for this scan, so there was never anything to issue.
   */
  no_cache,
  /**
   * The executor had already started reading this split: prefetching now would issue the same IO a
   * second time and race the reader for its chunks.
   */
  consumer_ahead,
  /**
   * The cache could not retain staging buffers through the point where IO was issued.  Normal
   * allocation pressure is reported earlier by prepare().
   */
  memory_pressure,
  /**
   * The cache refused for a reason of its own (already loading, request cancelled, backend cannot
   * serve vectored host reads).
   */
  other,
};

/**
 * How preparing one datasource's prefetch request turned out.  Kept apart from a plain bool because
 * "there was nothing to prepare" and "the pool had nothing to give" are opposite answers to "should
 * the readahead be worried".
 */
enum class prepare_result : std::uint8_t {
  /**
   * The request owns staging buffers and its chunks can now be claimed.
   */
  prepared,
  /**
   * The pool could not satisfy the request. It remains queued for a retry.
   */
  allocation_failed,
  /**
   * The consumer reached this split before preparation completed.
   */
  fallen_behind,
  /**
   * No request on this datasource: no prefetching cache, or no fadvise.
   */
  nothing_to_prepare,
};

/**
 * @brief One scan's datasource, forwarding reads to `ioctx` or its prefetching cache.
 *
 * Scans may share the underlying `io_object`, but each owns its own `sirius_datasource` and
 * `cache_handle`. Initialize `fadvise` before publishing the scan. Handle replacement, movement and
 * destruction require external exclusion from reads, prefetch operations and future waits accessing
 * the handle. A stable installed handle uses the cache's atomic producer/consumer protocol;
 * deferred batch mutexes do not protect handle replacement. Datasource destruction cancels deferred
 * issuance under its batch mutexes but must still exclude other datasource operations.
 */
class sirius_datasource : public cudf::io::datasource {
 public:
  explicit sirius_datasource(std::shared_ptr<ioctx> io_ctx, std::shared_ptr<io_object> io_obj);

  ~sirius_datasource() override;

  sirius_datasource(sirius_datasource const&)            = delete;
  sirius_datasource& operator=(sirius_datasource const&) = delete;

  // ---- Context accessors ---------------------------------------------------

  [[nodiscard]] std::shared_ptr<ioctx> io_ctx() const { return _io_ctx; }

  /**
   * The underlying io_object this datasource reads through.  Exposed so callers that received the
   * datasource from @c ioctx::open_datasource can still reach the io_object (e.g. as the
   * metadata-store cache key).
   */
  [[nodiscard]] const io_object& get_io_object() const noexcept { return *_io_object; }

  /**
   * Backend-parsed metadata for this datasource's io_object, looked up in the ioctx's metadata
   * store (null when no cache or no entry). Independent of the prefetching machinery.
   */
  [[nodiscard]] std::shared_ptr<io_object_metadata> metadata() const;

  [[nodiscard]] bool store_metadata(std::shared_ptr<io_object_metadata> metadata);

  // ---- cudf::io::datasource overrides ---------------------------------------

  [[nodiscard]] size_t size() const override;

  [[nodiscard]] bool supports_device_read() const override;

  [[nodiscard]] bool supports_vector_host_read() const;

  [[nodiscard]] bool is_device_read_preferred(size_t) const override;

  size_t host_read(size_t offset, size_t size, uint8_t* dst) override;

  std::unique_ptr<datasource::buffer> host_read(size_t offset, size_t size) override;

  std::future<size_t> host_read_async(size_t offset, size_t size, uint8_t* dst) override;

  std::future<std::unique_ptr<datasource::buffer>> host_read_async(size_t offset,
                                                                   size_t size) override;

  std::unique_ptr<datasource::buffer> device_read(size_t offset,
                                                  size_t size,
                                                  cudf_datasource_stream_t stream) override;
  size_t device_read(size_t offset,
                     size_t size,
                     uint8_t* dst,
                     cudf_datasource_stream_t stream) override;

  /**
   * @brief Read one range asynchronously into a borrowed device destination.
   *
   * With the prefetching cache and `deferred_device_reads` enabled, explicit-stream reads capture
   * their CUDA device and join a batch. The first `get()` or `wait()` issues the batch through
   * `device_read_ranges_async`; `deferred_batch_registry` also issues collected batches from other
   * datasources on that stream. Cache lookup, copies and backend I/O occur at issuance. Each member
   * returns its own clamped byte count. Shared default-stream handles and reads without this
   * optimization issue eagerly.
   *
   * A destination is accepted only if this call returns its future. Dropping one member preserves
   * its slice while other members remain. Last-member destruction and issuance arbitrate under the
   * batch mutex: if destruction wins, the unissued batch is cancelled; if issuance wins,
   * destinations remain borrowed until completion and destruction does not drain the I/O. A caller
   * reclaiming destinations after abandonment must externally exclude racing issuance. `wait_for()`
   * and `wait_until()` report `std::future_status::deferred` without issuing. Destroying the
   * datasource cancels unissued batches without allocating; a later member `get()` reports
   * `std::logic_error` (or allocation failure while constructing that error).
   */
  std::future<size_t> device_read_async(size_t offset,
                                        size_t size,
                                        uint8_t* dst,
                                        cudf_datasource_stream_t stream) override;

  /**
   * \brief Vectored form of @c device_read_async: read every range into its own device destination
   * in a single dispatch.
   *
   * \note Not a @c cudf::io::datasource override — cudf has no batched device read.  Callers
   * holding many ranges (e.g. a parquet scan's column chunks) should prefer this over one @c
   * device_read_async per range: it costs one request instead of N, and lets the backend fuse and
   * order the whole batch.
   */
  std::future<size_t> device_read_ranges_async(std::span<const slice> slices,
                                               ::cuda::stream_ref stream);

  std::future<size_t> host_read_ranges_async(std::span<const slice> slices);

  // ---- Advisory IO ---------------------------------------------------------

  /**
   * \brief Return a fresh datasource that shares this one's @c ioctx and @c io_object (so it points
   * at the same file) but carries an empty @c cache_handle.
   *
   * \note Used when a single file is split across multiple scans (e.g. several row_group_slices
   * from the same parquet file).  Each split owns its own datasource via @c duplicate so it can
   * call @c fadvise without stomping on another scan's handle — io_objects are deliberately
   * shareable across datasources, but handles are not.
   */
  [[nodiscard]] std::unique_ptr<sirius_datasource> duplicate() const;

  /**
   * \brief Hint the IO layer about @p ranges that this scan will (or might) read soon.
   *
   * Hands @p ranges to the prefetching cache, stashes the returned @c cache_handle on this
   * datasource (which disposes the request when it goes away) and drives it to @c
   * scan_stage::initialized.  No-op when the cache is unavailable.  A second inserting call while
   * an active handle is already stored is a caller bug and only logs a warning: the datasource
   * lifecycle expects one insert per scan.
   */
  void fadvise(std::span<const cudf::io::text::byte_range_info> ranges, std::optional<int> dev_id);

  /**
   * Drive the stashed handle's consumer stage to @p site.
   */
  void update(cache::scan_stage site);

  /**
   * Allocate staging buffers for the stashed request, ahead of prefetching it. @p wait_for_eviction
   * lets the call wait on the evictor rather than fail on a momentarily empty pool.  See @c
   * prefetching_cache::prepare.
   */
  prepare_result prepare_prefetch(bool wait_for_eviction);

  /**
   * Issue prefetch IO for the stashed handle.  @p on_done fires exactly once with the outcome —
   * inline when no IO is issued, otherwise from the IO completion.  Returns @c
   * prefetch_refusal::issued when IO went out, and otherwise why it did not.
   */
  prefetch_refusal prefetch_async(exec::invocable<void(bool) noexcept> on_done);

  /**
   * Why this datasource's prefetch failed, if it did: null otherwise, and null when no prefetch was
   * ever issued.  Set before the prefetch's completion callback fires and before @c
   * wait_until_ready would return.
   */
  [[nodiscard]] std::exception_ptr prefetch_failure() const noexcept;

  [[nodiscard]] bool uses_prefetching_cache() const noexcept;

  /**
   * Whether the backend serving this datasource would rather be handed one batched request than a
   * stream of small reads.  See @c ioctx::prefers_bulk_io.
   */
  [[nodiscard]] bool prefers_bulk_io() const noexcept;

 private:
  /**
   * @brief Construct a member future before publishing its borrowed destination to an open batch.
   */
  [[nodiscard]] std::future<size_t> enqueue_deferred(cudaStream_t stream, slice const& request);

  std::shared_ptr<ioctx> _io_ctx;
  std::shared_ptr<io_object> _io_object;
  /**
   * Handle of the most recent insert into the prefetching cache, or empty if none was made.
   * Disposing it lets the cache reclaim the request.
   */
  cache::cache_handle _cache_handle;

  /**
   * Guards @c _pending_batches. Lock order: this mutex before a batch or registry mutex; registry
   * operations never take either of the other mutexes.
   */
  std::mutex _pending_mutex;
  /**
   * Batches opened by `device_read_async`, at most one open batch per stream and device. Weak
   * references do not keep batches alive; last-member destruction closes an unissued batch even if
   * an issuer retains a strong reference.
   */
  std::vector<std::weak_ptr<deferred_device_batch>> _pending_batches;
};

}  // namespace sirius::io
