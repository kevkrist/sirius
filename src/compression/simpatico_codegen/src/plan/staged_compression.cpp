// SPDX-License-Identifier: Apache-2.0
//
// The staged owning compress driver.
//
// try_compress_with_plan() owns the original table while the ordinary plan walk builds a
// self-contained candidate. Root projections may avoid intermediate identity/str_split copies
// when their channels feed materializing codecs, but candidate leaves always own their storage.
//
// The original is therefore never dismantled. reject() returns its exact allocations; accept()
// proves stream completion, discards it, and returns the already-owning candidate. This narrower
// zero-copy boundary is deliberate: pinned cuDF has no allocation-free column detach primitive,
// so transferring root components at accept time cannot provide a genuine no-fail commit.
#include "api/simpatico_codegen.hpp"
#include "codegen/plan/plan_interpreter.hpp"
#include "compress_internals.hpp"

#include <cudf/utilities/error.hpp>

#include <rmm/cuda_device.hpp>
#include <rmm/error.hpp>

#include <cuda_runtime.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace simpatico {

// ── staged_compression ────────────────────────────────────────────────────────

namespace detail {
thread_local completion_probe stage_completion_probe = nullptr;
}  // namespace detail

/// An active stage. The stage is empty exactly when it has no impl, so there is no separate
/// "decided" flag to keep in step: accept()/reject()/the move constructor all drop it.
struct staged_compression::impl {
  std::unique_ptr<cudf::table> original;  ///< whole and valid until the decision
  compressed_table candidate;
  rmm::cuda_device_id device{0};
  rmm::cuda_stream_view stream{cudf::get_default_stream()};
  std::string error;
  bool ready = false;
};

static_assert(std::is_nothrow_move_constructible_v<compressed_table>);
static_assert(std::is_nothrow_destructible_v<compressed_table>);

namespace {

cudaError_t probe_completion(rmm::cuda_stream_view stream) noexcept
{
  return detail::stage_completion_probe ? detail::stage_completion_probe(stream)
                                        : cudaStreamSynchronize(stream.value());
}

// Prove that everything the stage put on the stream is finished — including reads the caller
// launched off table() while inspecting the candidate — before any ownership changes hands.
// Waiting at the stream's *current* tail, rather than at an event recorded when the stage was
// built, is what makes it safe to stage payload bytes out of a live stage.
void require_completion(rmm::cuda_stream_view stream, char const* decision)
{
  cudaError_t const status = probe_completion(stream);
  if (status != cudaSuccess) {
    throw detail::plan_error(std::string("staged_compression::") + decision +
                             ": cannot prove GPU completion (" + cudaGetErrorString(status) +
                             "); the stage is unchanged and still decidable");
  }
}

// A failed stage is recoverable only after all work that might still reference either outcome is
// known complete. If that proof fails, returning ok()==false would make reject/destruction look
// safe when it is not, so escalate as a fatal CUDA error instead.
void require_recovery_completion(rmm::cuda_stream_view stream, rmm::cuda_device_id device_id)
{
  rmm::cuda_set_device_raii device{device_id};
  cudaError_t const status = probe_completion(stream);
  if (status != cudaSuccess) {
    throw cudf::fatal_cuda_error{
      std::string{"try_compress_with_plan: cannot establish rollback quiescence ("} +
        cudaGetErrorString(status) + ")",
      status};
  }
}
}  // namespace

staged_compression::staged_compression(std::unique_ptr<impl> state) noexcept
  : state_(std::move(state))
{
}

staged_compression::staged_compression(staged_compression&&) noexcept = default;

staged_compression::~staged_compression() noexcept
{
  if (!state_) return;
  auto retain = [&] { [[maybe_unused]] auto* retained = state_.release(); };

  // Destroy both outcomes only on their construction device and after stream completion.
  // Failure in either proof deliberately leaks them rather than returning live storage to an
  // allocator. This path is non-allocating and safe while another exception is unwinding.
  try {
    rmm::cuda_set_device_raii device{state_->device};
    if (probe_completion(state_->stream) != cudaSuccess) {
      std::fputs(
        "simpatico: staged_compression abandoned without provable GPU completion; retaining "
        "its device storage\n",
        stderr);
      retain();
      return;
    }
    state_.reset();
  } catch (...) {
    std::fputs(
      "simpatico: staged_compression could not enter its CUDA device; retaining device storage\n",
      stderr);
    retain();
  }
}

bool staged_compression::ok() const noexcept { return state_ != nullptr && state_->ready; }

std::string const& staged_compression::error() const& noexcept
{
  static std::string const none;
  return state_ ? state_->error : none;
}

compressed_table_inspection staged_compression::table() const&
{
  if (!ok()) throw std::logic_error("staged_compression::table() requires a ready stage");
  return compressed_table_inspection{state_->candidate, state_->stream};
}

compressed_table staged_compression::accept() &&
{
  if (!ok()) throw std::logic_error("staged_compression::accept() requires a ready stage");
  impl& stage = *state_;

  // The candidate already owns every byte it exposes. Completion is the only fallible step;
  // after it succeeds, accepting is just nothrow moves and destruction. A completion failure
  // leaves both outcomes active and unchanged.
  rmm::cuda_set_device_raii device{stage.device};
  require_completion(stage.stream, "accept");

  compressed_table accepted = std::move(stage.candidate);
  state_.reset();
  return accepted;
}

std::unique_ptr<cudf::table> staged_compression::reject() &&
{
  if (!state_) throw std::logic_error("staged_compression::reject() on an empty stage");
  impl& stage = *state_;
  rmm::cuda_set_device_raii device{stage.device};
  require_completion(stage.stream, "reject");

  // The candidate owns only its own outputs; the original was never taken apart.
  auto original = std::move(stage.original);
  state_.reset();
  return original;
}

staged_compression try_compress_with_plan(std::unique_ptr<cudf::table> table,
                                          std::string_view plan_dsl,
                                          rmm::cuda_stream_view stream,
                                          rmm::device_async_resource_ref mr,
                                          std::vector<std::string> column_names)
{
  // Build the shell first. Ownership passed at the call site, so a host failure before the
  // stage exists is the one window rollback cannot cover (documented on staged_compression).
  // Past this point every failure becomes a failed stage whose reject() still works.
  auto state      = std::make_unique<staged_compression::impl>();
  state->device   = rmm::get_current_cuda_device();
  state->stream   = stream;
  state->original = std::move(table);
  staged_compression stage{std::move(state)};
  staged_compression::impl& s = *stage.state_;

  try {
    if (!s.original) throw detail::plan_error("try_compress_with_plan: null table");
    auto const plans = detail::split_plan_dsl_impl(plan_dsl);
    detail::validate_plan_count(plans.size(), s.original->num_columns());
    detail::validate_column_names(column_names, plans.size());

    s.candidate.columns.reserve(plans.size());
    for (std::size_t i = 0; i < plans.size(); ++i) {
      cudf::column& root = s.original->get_column(static_cast<cudf::size_type>(i));

      // Projections borrow only while the walk is building owning outputs; the root remains a
      // structurally valid column throughout.
      staged_root_context context{};
      std::string error;
      auto compound = compress_column(root.view(), plans[i], stream, mr, &error, &context);
      if (!compound) throw detail::plan_error(error.empty() ? "compress failed" : error);

      compressed_column column;
      column.dtype    = root.type();
      column.num_rows = s.original->num_rows();
      column.compound = std::move(compound);
      if (!column_names.empty()) column.name = column_names[i];
      s.candidate.columns.push_back(std::move(column));
    }
    s.ready = true;
  } catch (cudf::fatal_cuda_error const&) {
    throw;
  } catch (rmm::bad_alloc const& failure) {
    require_recovery_completion(stream, s.device);
    s.error = failure.what();
  } catch (std::bad_alloc const&) {
    // Host allocation failure cannot reliably allocate a failed-stage diagnostic.
    throw;
  } catch (std::exception const& failure) {
    require_recovery_completion(stream, s.device);
    s.error = failure.what();
  } catch (...) {
    require_recovery_completion(stream, s.device);
    s.error = "compression failed with an unknown exception";
  }

  if (!s.ready) {
    // Quiescence was established in the catch path, so partial owning outputs can be released.
    s.candidate.columns.clear();
    if (s.error.empty()) s.error = "compress failed";
  }
  return stage;
}

}  // namespace simpatico
