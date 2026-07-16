// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "codegen/plan/plan_interpreter.hpp"
#include "codegen/util/stream_pool.hpp"

#include <cudf/table/table.hpp>
#include <cudf/table/table_view.hpp>
#include <cudf/types.hpp>
#include <cudf/utilities/default_stream.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace simpatico {

/// A single column after compression: metadata plus the compressed plan compound.
struct compressed_column {
  /// Optional name copied from the source table (populated when column names are
  /// passed to compress_with_plan).
  std::optional<std::string> name;
  /// Original element type of the column.
  cudf::data_type dtype{};
  /// Number of logical rows in the original column.
  std::int64_t num_rows = 0;
  /// Plan tree describing the compression layout; owns every compressed rep.
  std::unique_ptr<PlanTree> compound;
};

/// Compressed representation of a cuDF table: one compressed_column per source
/// column, in the same order, with optional per-column names.
class compressed_table {
 public:
  std::vector<compressed_column> columns;

  /// Number of compressed columns.
  std::size_t num_columns() const { return columns.size(); }

  /// Common row count across all columns (0 if the table is empty).
  std::int64_t num_rows() const;

  /// Return a flat leaf descriptor for every stored representation in each
  /// column. The outer vector is indexed by column; the inner by leaf within
  /// that column. Leaf order follows PlanTree node order (node.rep before
  /// node.channels; channels in output_paths order where available).
  std::vector<std::vector<simpatico::leaf_desc>> describe(
    rmm::cuda_stream_view stream = cudf::get_default_stream()) const;

  /// Decompress on a single CUDA stream.
  ///
  /// Equivalent to calling the free function simpatico::decompress(*this, ...).
  std::unique_ptr<cudf::table> decompress(
    rmm::cuda_stream_view stream      = cudf::get_default_stream(),
    rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref()) const&;

  /// Destructive single-stream decode. Transferable reps move their buffers;
  /// consumed reps then fail loudly. Not failure-atomic after transfers begin.
  std::unique_ptr<cudf::table> decompress(
    rmm::cuda_stream_view stream      = cudf::get_default_stream(),
    rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref()) &&;
};

struct payload_buffer_ref;
class staged_compression;

/// Borrowed, read-only access to a staged candidate.
///
/// Unlike `compressed_table const&`, this facade does not expose the public `columns` vector.
/// Its GPU operations are pinned to the transaction stream so a decision's synchronization
/// necessarily covers them. The facade itself is copyable and must not outlive its stage.
class compressed_table_inspection {
 public:
  std::size_t num_columns() const noexcept { return table_->num_columns(); }
  std::int64_t num_rows() const { return table_->num_rows(); }
  rmm::cuda_stream_view stream() const noexcept { return stream_; }

  std::vector<std::vector<simpatico::leaf_desc>> describe() const
  {
    return table_->describe(stream_);
  }

  std::unique_ptr<cudf::table> decompress(
    rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref()) const
  {
    return table_->decompress(stream_, mr);
  }

 private:
  compressed_table_inspection(compressed_table const& table, rmm::cuda_stream_view stream) noexcept
    : table_(&table), stream_(stream)
  {
  }

  friend class staged_compression;
  friend std::string build_compressed_table_header(compressed_table_inspection table,
                                                   std::vector<std::uint8_t>& out_header,
                                                   std::vector<payload_buffer_ref>& out_buffers,
                                                   std::uint64_t& out_payload_bytes);

  compressed_table const* table_;
  rmm::cuda_stream_view stream_;
};

// ── Utilities ─────────────────────────────────────────────────────────────────

/// Split a multi-column plan DSL string on `---` separator lines.
///
/// Lines that are blank or begin with `#` are skipped. Each resulting block is
/// trimmed of leading/trailing whitespace. The returned vector contains one
/// entry per column plan, in order.
///
/// @param plan_dsl  Multi-column plan string in Simpatico DSL format.
std::vector<std::string> split_plan_dsl(std::string_view plan_dsl);

// ── Compression ───────────────────────────────────────────────────────────────

/// Compress all columns of @p table sequentially on a single CUDA stream.
///
/// @param table         Source table. Column count must equal the number of
///                      `---`-separated blocks in @p plan_dsl.
/// @param plan_dsl      Multi-column plan DSL string.
/// @param stream        CUDA stream used for all GPU operations.
/// @param mr            Device memory resource; nullptr selects the RMM default.
/// @param column_names  Optional per-column names stored in the result. Must be
///                      empty or exactly @p table.num_columns() long.
/// @throws std::runtime_error  plan/table column count mismatch or GPU error.
compressed_table compress_with_plan(
  cudf::table_view table,
  std::string_view plan_dsl,
  rmm::cuda_stream_view stream          = cudf::get_default_stream(),
  rmm::device_async_resource_ref mr     = rmm::mr::get_current_device_resource_ref(),
  std::vector<std::string> column_names = {});

/// Compress all columns in parallel using @p column_threads worker threads.
///
/// An internal stream_pool of size `max(1, column_threads)` is created for the
/// call. The returned plan trees share ownership of it, so every allocation's
/// deallocation stream remains valid until the compressed storage is destroyed.
///
/// @param table          Source table.
/// @param plan_dsl       Multi-column plan DSL string.
/// @param column_threads Number of parallel CUDA streams / worker threads.
/// @param mr             Device memory resource; nullptr selects the RMM default.
/// @param column_names   Optional per-column names.
/// @throws std::runtime_error  plan/table column count mismatch or GPU error.
compressed_table compress_with_plan(
  cudf::table_view table,
  std::string_view plan_dsl,
  int column_threads,
  rmm::device_async_resource_ref mr     = rmm::mr::get_current_device_resource_ref(),
  std::vector<std::string> column_names = {});

/// Compress all columns in parallel using a caller-owned stream pool.
///
/// The pool must outlive the returned compressed table and all of its stored
/// representations; do not move, shut down, or destroy it while that storage
/// exists. Reusing one sufficiently long-lived pool across calls avoids repeated
/// stream allocation.
///
/// @param table          Source table.
/// @param plan_dsl       Multi-column plan DSL string.
/// @param pool           Caller-supplied stream pool.
/// @param mr             Device memory resource; nullptr selects the RMM default.
/// @param column_names   Optional per-column names.
/// @throws std::runtime_error  plan/table column count mismatch or GPU error.
compressed_table compress_with_plan(
  cudf::table_view table,
  std::string_view plan_dsl,
  simpatico::stream_pool& pool,
  rmm::device_async_resource_ref mr     = rmm::mr::get_current_device_resource_ref(),
  std::vector<std::string> column_names = {});

// ── Staged owning compression ─────────────────────────────────────────────────

/// A move-only compression transaction with exactly one decision.
///
/// The original table remains structurally intact. The candidate owns every buffer it exposes;
/// identity and str_split may lend root views to downstream materializing codecs while building
/// it, but bare representations and terminal channels are copied. This is the strongest safe
/// boundary available with the pinned cuDF API, which has no allocation-free column detach.
///
///   * accept() proves GPU completion, discards the original, and returns the owning candidate.
///   * reject() proves GPU completion, drops the candidate, and returns the original table with
///     the exact original allocations.
///
/// Three states. **ready** has both outcomes; **failed** has an error and the intact original;
/// **empty** is moved-from or decided. ok() is true exactly in ready. table() and accept() require
/// ready; reject() accepts either active state. Recoverable plan/operator/device-allocation
/// failures become failed stages only after stream quiescence is established. Fatal CUDA errors,
/// inability to establish rollback quiescence, and host allocation failure escape.
///
/// table() returns a read-only inspection facade bound to the stage stream. It and any payload
/// pointer derived from it are valid only while the stage is active and must not cross a decision.
/// Payload reads must be enqueued on inspection.stream(). accept(), reject(), and destruction wait
/// at that stream's current tail, covering work queued after table(). Destroying an active stage
/// abandons both outcomes rather than choosing one.
///
/// The stage owns neither stream nor memory resources. The supplied stream/MR and every resource
/// backing the original must outlive the active stage. Candidate resources must additionally
/// outlive an accepted table; original resources must outlive a rejected table. Decisions and
/// destruction re-enter the CUDA device that was current when the stage was created.
///
/// Not thread-safe.
class staged_compression {
 public:
  staged_compression(staged_compression const&)            = delete;
  staged_compression& operator=(staged_compression const&) = delete;
  staged_compression(staged_compression&&) noexcept;
  /// Deleted: assigning over an active transaction would silently discard its decision.
  staged_compression& operator=(staged_compression&&) = delete;
  ~staged_compression() noexcept;

  /// True exactly in the ready state. The only observer that is meaningful on an empty stage.
  bool ok() const noexcept;
  /// Empty in ready/empty, the failure reason in failed.
  std::string const& error() const& noexcept;
  std::string const& error() const&& = delete;

  /// Read-only inspection of the compressed candidate; valid only while this stage is active.
  /// @throws std::logic_error  unless the stage is ready.
  compressed_table_inspection table() const&;
  compressed_table_inspection table() const&& = delete;

  /// Keep the compressed form. Requires ready.
  /// @throws std::logic_error  if the stage is not ready.
  /// @throws std::runtime_error  if GPU completion cannot be established — the stage is left
  ///                             active and unchanged, and remains decidable.
  [[nodiscard]] compressed_table accept() &&;

  /// Give the original table back, unchanged. Permitted in ready or failed.
  /// @throws std::logic_error  if the stage is already empty.
  /// @throws std::runtime_error  if GPU completion cannot be established — the stage is left
  ///                             active and unchanged, and remains decidable.
  [[nodiscard]] std::unique_ptr<cudf::table> reject() &&;

 private:
  struct impl;
  explicit staged_compression(std::unique_ptr<impl> state) noexcept;
  friend staged_compression try_compress_with_plan(std::unique_ptr<cudf::table>,
                                                   std::string_view,
                                                   rmm::cuda_stream_view,
                                                   rmm::device_async_resource_ref,
                                                   std::vector<std::string>);
  std::unique_ptr<impl> state_;
};

/// Compress an owned table into a reversible, single-stream transaction.
///
/// Every prior asynchronous use of @p table, read or write, must happen-before work on @p stream;
/// no other stream may keep using its storage after ownership is transferred. Root storage may be
/// read directly while materializing codecs, but the candidate owns every byte it exposes. The
/// original remains intact for reject(). A null table produces a failed stage whose reject()
/// returns null.
///
/// @param table         Source table; ownership passes to the returned stage.
/// @param plan_dsl      Multi-column plan DSL string.
/// @param stream        CUDA stream used for all GPU operations, and for proving completion.
/// @param mr            Device memory resource.
/// @param column_names  Optional per-column names; empty or exactly table->num_columns() long.
/// @returns  A ready stage, or a failed one carrying the reason. Either way the original table
///           is intact and reject() returns it.
/// @throws cudf::fatal_cuda_error  fatal CUDA failure or inability to establish rollback
///                                 quiescence.
/// @throws std::bad_alloc          host allocation failure while capturing the transaction.
/// @note If an exception escapes, no rejectable stage exists. The input is destroyed after
///       quiescence, or deliberately retained if safe destruction cannot be proved.
[[nodiscard]] staged_compression try_compress_with_plan(
  std::unique_ptr<cudf::table> table,
  std::string_view plan_dsl,
  rmm::cuda_stream_view stream          = cudf::get_default_stream(),
  rmm::device_async_resource_ref mr     = rmm::mr::get_current_device_resource_ref(),
  std::vector<std::string> column_names = {});

// ── Decompression ─────────────────────────────────────────────────────────────

/// Decompress a single compressed column.
///
/// @param compound  Compressed column payload.
/// @param stream    CUDA stream for all GPU operations.
/// @param mr        Device memory resource; nullptr selects the RMM default.
/// @returns  Newly allocated decompressed column.
/// @throws std::runtime_error on GPU error.
std::unique_ptr<cudf::column> decompress(
  const PlanTree& tree,
  rmm::cuda_stream_view stream      = cudf::get_default_stream(),
  rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref());

/// Decompress all columns of @p table sequentially on a single CUDA stream.
///
/// @param table   Compressed table.
/// @param stream  CUDA stream for all GPU operations.
/// @param mr      Device memory resource; nullptr selects the RMM default.
/// @returns  Newly allocated decompressed table (column order matches input).
/// @throws std::runtime_error on GPU error.
std::unique_ptr<cudf::table> decompress(
  const compressed_table& table,
  rmm::cuda_stream_view stream      = cudf::get_default_stream(),
  rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref());

/// Destructive single-stream decode. The source remains valid, but reps whose
/// storage transfers cannot be reused or described. Transferred buffers are
/// rebound to @p stream for eventual deallocation, so that stream must outlive
/// the result. Not failure-atomic.
std::unique_ptr<cudf::table> decompress(
  compressed_table&& table,
  rmm::cuda_stream_view stream      = cudf::get_default_stream(),
  rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref());

/// Decompress all columns in parallel using @p column_threads worker threads.
///
/// The internal worker pool is synchronized before return. Returned buffers are
/// rebound to the process-lifetime cuDF default stream before that pool is
/// destroyed.
///
/// @param table          Compressed table.
/// @param column_threads Number of parallel CUDA streams / worker threads.
/// @param mr             Device memory resource; nullptr selects the RMM default.
/// @throws std::runtime_error on GPU error.
std::unique_ptr<cudf::table> decompress(
  const compressed_table& table,
  int column_threads,
  rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref());

std::unique_ptr<cudf::table> decompress(
  compressed_table&& table,
  int column_threads,
  rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref()) = delete;

/// Decompress all columns in parallel using a caller-owned stream pool.
///
/// The pool must outlive the returned table because its streams remain the
/// deallocation streams of the returned column buffers. Do not move, shut down,
/// or destroy it while the result exists.
///
/// @param table  Compressed table.
/// @param pool   Caller-supplied stream pool.
/// @param mr     Device memory resource; nullptr selects the RMM default.
/// @throws std::runtime_error on GPU error.
std::unique_ptr<cudf::table> decompress(
  const compressed_table& table,
  simpatico::stream_pool& pool,
  rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref());

std::unique_ptr<cudf::table> decompress(
  compressed_table&& table,
  simpatico::stream_pool& pool,
  rmm::device_async_resource_ref mr = rmm::mr::get_current_device_resource_ref()) = delete;

}  // namespace simpatico
