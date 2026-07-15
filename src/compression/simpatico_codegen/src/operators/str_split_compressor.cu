// SPDX-License-Identifier: Apache-2.0
//
// str_split: decompose STRING into {offsets, chars, null_mask}; reassemble via
// make_strings_column on decode. Structural operator.

#include "codegen/plan/bitjoin_layout.hpp"  // copy_column_view
#include "codegen/plan/representation.hpp"

#include <cudf/column/column.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/strings/strings_column_view.hpp>
#include <cudf/types.hpp>
#include <cudf/utilities/error.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace simpatico {

namespace {

bool is_str_split_chars_type(cudf::data_type type)
{
  auto const id = type.id();
  return id == cudf::type_id::UINT8 || id == cudf::type_id::UINT32 || id == cudf::type_id::UINT64;
}

void validate_str_split_storage(str_split_compressed_representation const& rep)
{
  if (rep.num_rows < 0 || rep.null_count_ < -1 || rep.null_count_ > rep.num_rows) {
    throw std::logic_error("str_split representation has invalid row/null counts");
  }
  if (!rep.offsets_) {
    throw std::logic_error("str_split representation is missing offsets storage");
  }

  auto const offsets_id = rep.offsets_->type().id();
  if ((offsets_id != cudf::type_id::INT32 && offsets_id != cudf::type_id::INT64) ||
      static_cast<std::int64_t>(rep.offsets_->size()) !=
        static_cast<std::int64_t>(rep.num_rows) + 1 ||
      rep.offsets_->nullable() || rep.offsets_->num_children() != 0) {
    throw std::logic_error("str_split representation has invalid offsets storage");
  }

  if (rep.chars_size_ < 0 || !is_str_split_chars_type(rep.chars_type_)) {
    throw std::logic_error("str_split representation has invalid chars metadata");
  }
  auto const chars_bytes = static_cast<std::size_t>(rep.chars_size_) *
                           static_cast<std::size_t>(cudf::size_of(rep.chars_type_));
  if (rep.chars_.size() < chars_bytes || (chars_bytes != 0 && rep.chars_.data() == nullptr)) {
    throw std::logic_error("str_split representation has invalid chars storage");
  }

  if (rep.has_null_mask_) {
    auto const required = cudf::bitmask_allocation_size_bytes(rep.num_rows);
    if (rep.null_mask_size_ < 0 || static_cast<std::size_t>(rep.null_mask_size_) < required ||
        rep.null_mask_.size() < static_cast<std::size_t>(rep.null_mask_size_) ||
        (rep.null_mask_size_ != 0 && rep.null_mask_.data() == nullptr)) {
      throw std::logic_error("str_split representation has invalid null-mask storage");
    }
  } else if (rep.null_count_ != 0 || rep.null_mask_size_ != 0) {
    throw std::logic_error("str_split representation has null metadata but no null-mask storage");
  }
}

cudf::size_type resolve_null_count(str_split_compressed_representation const& rep,
                                   rmm::cuda_stream_view stream)
{
  if (rep.null_count_ >= 0) return rep.null_count_;
  auto const* bits = reinterpret_cast<cudf::bitmask_type const*>(rep.null_mask_.data());
  return cudf::null_count(bits, 0, rep.num_rows, stream);
}

}  // namespace

str_split_compressed_representation::str_split_compressed_representation(
  cudf::size_type n_rows,
  std::unique_ptr<cudf::column> offsets,
  std::unique_ptr<cudf::column> chars,
  std::unique_ptr<cudf::column> null_mask,
  cudf::size_type null_count)
  : compressed_representation(cudf::data_type{cudf::type_id::STRING}, n_rows),
    null_count_(null_count)
{
  if (!offsets || !chars) {
    throw std::invalid_argument("str_split representation requires offsets and chars");
  }
  if (chars->nullable() || chars->num_children() != 0 ||
      (null_mask && (null_mask->type().id() != cudf::type_id::UINT8 || null_mask->nullable() ||
                     null_mask->num_children() != 0))) {
    throw std::invalid_argument("str_split channels must be flat and non-nullable");
  }

  chars_type_     = chars->type();
  chars_size_     = chars->size();
  null_mask_size_ = null_mask ? null_mask->size() : 0;
  has_null_mask_  = null_mask != nullptr;
  offsets_        = std::move(offsets);

  auto chars_contents = chars->release();
  chars_ = chars_contents.data ? std::move(*chars_contents.data) : rmm::device_buffer{};

  if (null_mask) {
    auto mask_contents = null_mask->release();
    null_mask_         = mask_contents.data ? std::move(*mask_contents.data) : rmm::device_buffer{};
  }
  validate_str_split_storage(*this);
}

std::unique_ptr<cudf::column> str_split_compressed_representation::decompress(
  rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr) const
{
  ensure_not_consumed();
  auto const null_count = resolve_null_count(*this, stream);

  auto offsets_copy = std::make_unique<cudf::column>(*offsets_, stream, mr);
  rmm::device_buffer chars_copy(chars_, stream, mr);
  rmm::device_buffer mask_copy{};
  if (null_count > 0) { mask_copy = rmm::device_buffer(null_mask_, stream, mr); }

  return cudf::make_strings_column(
    num_rows, std::move(offsets_copy), std::move(chars_copy), null_count, std::move(mask_copy));
}

std::unique_ptr<cudf::column> str_split_compressed_representation::take_decompressed(
  rmm::cuda_stream_view stream, rmm::device_async_resource_ref)
{
  ensure_not_consumed();
  validate_str_split_storage(*this);
  auto const null_count = resolve_null_count(*this, stream);

  consumed_    = true;
  auto offsets = std::move(offsets_);
  auto chars   = std::move(chars_);
  rmm::device_buffer null_mask{};
  if (null_count > 0) { null_mask = std::move(null_mask_); }

  return cudf::make_strings_column(
    num_rows, std::move(offsets), std::move(chars), null_count, std::move(null_mask));
}

std::vector<compressible_output> str_split_compressed_representation::named_channels(
  rmm::cuda_stream_view) const
{
  ensure_not_consumed();

  std::vector<compressible_output> channels;
  channels.reserve(has_null_mask_ && null_count_ != 0 ? 3 : 2);
  channels.push_back({"offsets", offsets_->view()});
  channels.push_back(
    {"chars", cudf::column_view{chars_type_, chars_size_, chars_.data(), nullptr, 0}});
  if (has_null_mask_ && null_count_ != 0) {
    channels.push_back(
      {"null_mask",
       cudf::column_view{
         cudf::data_type{cudf::type_id::UINT8}, null_mask_size_, null_mask_.data(), nullptr, 0}});
  }
  return channels;
}

std::vector<std::string> str_split_compressed_representation::required_channels() const
{
  ensure_not_consumed();
  return has_null_mask_ && null_count_ != 0 ? std::vector<std::string>{"null_mask"}
                                            : std::vector<std::string>{};
}

std::unique_ptr<compressed_representation> str_split_compressor::compress(
  cudf::column_view column_to_compress,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  if (column_to_compress.type().id() != cudf::type_id::STRING) {
    throw std::runtime_error("str_split_compressor: column must be STRING, got '" +
                             type_id_to_name(column_to_compress.type()) + "'");
  }

  if (column_to_compress.size() == 0) {
    // Canonical empty rep: one zero offset and no chars, built directly rather
    // than copied from the input — the canonical empty STRING column,
    // cudf::make_empty_column(STRING), has no offsets child to copy from.
    auto offsets = cudf::make_fixed_width_column(
      cudf::data_type{cudf::type_id::INT32}, 1, cudf::mask_state::UNALLOCATED, stream, mr);
    CUDF_CUDA_TRY(cudaMemsetAsync(
      offsets->mutable_view().head<void>(), 0, sizeof(std::int32_t), stream.value()));
    auto chars = cudf::make_fixed_width_column(
      cudf::data_type{cudf::type_id::UINT8}, 0, cudf::mask_state::UNALLOCATED, stream, mr);
    CUDF_CUDA_TRY(cudaStreamSynchronize(stream.value()));
    return std::make_unique<str_split_compressed_representation>(
      0, std::move(offsets), std::move(chars), nullptr, 0);
  }

  std::unique_ptr<cudf::column>
    owned;  ///< Owned copy of the input column, if needed (post-gather).
  cudf::column_view src = column_to_compress;
  // Normalize any sliced view to an owned compact copy: a non-zero offset
  // needs rebasing, and a head-slice (offset 0, size < parent) still views the
  // parent's full offsets child, so the emitted channels would be mutually
  // inconsistent (offsets/chars parent-sized, null_mask slice-sized).
  if (column_to_compress.offset() != 0 ||
      cudf::strings_column_view(column_to_compress).offsets().size() !=
        column_to_compress.size() + 1) {
    owned = copy_column_view(column_to_compress, stream, mr);
    src   = owned->view();
  }

  cudf::strings_column_view scv(src);
  auto const n          = src.size();
  auto const null_count = src.nullable() ? src.null_count() : 0;

  // offsets: owned copy, INT32 for normal strings or INT64 for cudf "large strings"
  // (chars > 2GB); copy_column_view preserves the child's type.
  auto offsets = copy_column_view(scv.offsets(), stream, mr);

  // chars: a fixed-width column caps at 2^31 ELEMENTS, so >2GB chars can't be UINT8.
  // Widen the element type (bytes = elements x sizeof) to fit under the cap; byte
  // codecs are type-agnostic and decompress reads the raw buffer back via offsets.
  std::int64_t const chars_bytes = scv.chars_size(stream);
  std::int64_t const kElemCap    = std::numeric_limits<cudf::size_type>::max();
  cudf::type_id chars_tid        = cudf::type_id::UINT8;
  std::int64_t bpe               = 1;
  if (chars_bytes > kElemCap) { chars_tid = cudf::type_id::UINT32, bpe = 4; }
  if (chars_bytes > kElemCap * 4) { chars_tid = cudf::type_id::UINT64, bpe = 8; }
  if (chars_bytes > kElemCap * 8) {
    throw std::runtime_error("str_split_compressor: chars > 16GB out of scope");
  }
  auto const nelem = static_cast<cudf::size_type>((chars_bytes + bpe - 1) / bpe);
  auto chars       = cudf::make_fixed_width_column(
    cudf::data_type(chars_tid), nelem, cudf::mask_state::UNALLOCATED, stream, mr);
  if (chars_bytes > 0) {
    auto* dst = chars->mutable_view().head<std::uint8_t>();
    CUDF_CUDA_TRY(cudaMemcpyAsync(dst,
                                  scv.chars_begin(stream),
                                  static_cast<size_t>(chars_bytes),
                                  cudaMemcpyDeviceToDevice,
                                  stream.value()));
    std::int64_t const padded = static_cast<std::int64_t>(nelem) * bpe;
    if (padded > chars_bytes) {
      CUDF_CUDA_TRY(cudaMemsetAsync(
        dst + chars_bytes, 0, static_cast<size_t>(padded - chars_bytes), stream.value()));
    }
  }

  // Copy validity for nulls or an unknown count; known zero-null columns use two channels.
  std::unique_ptr<cudf::column> null_mask;
  if (src.nullable() && null_count != 0) {
    rmm::device_buffer mbuf = cudf::copy_bitmask(src, stream, mr);
    auto const mbytes       = static_cast<cudf::size_type>(cudf::bitmask_allocation_size_bytes(n));
    null_mask               = std::make_unique<cudf::column>(
      cudf::data_type{cudf::type_id::UINT8}, mbytes, std::move(mbuf), rmm::device_buffer{}, 0);
  }
  CUDF_CUDA_TRY(cudaStreamSynchronize(stream.value()));

  return std::make_unique<str_split_compressed_representation>(
    n, std::move(offsets), std::move(chars), std::move(null_mask), null_count);
}

std::optional<staged_root_projection> str_split_compressor::project_staged_root(
  cudf::column_view column, rmm::cuda_stream_view stream, rmm::device_async_resource_ref) const
{
  // Decline every shape the copying compress() path normalizes, and let it stay the fallback:
  // a sliced or head-sliced view needs a rebasing gather before its channels are mutually
  // consistent; an empty column has no offsets child to borrow; and chars past the cudf column
  // cap would need a widened element type whose trailing padding this projection cannot prove
  // initialized (compress() memsets it — a borrow cannot).
  // A real null mask also falls back: this projection is entirely borrowed, while the candidate
  // must own any terminal mask; moreover, cuDF specifies logical mask bits but not serialized
  // padding. The ordinary copy path preserves the existing v10 behavior.
  if (column.type().id() != cudf::type_id::STRING || column.size() == 0) return std::nullopt;
  if (column.offset() != 0 ||
      cudf::strings_column_view(column).offsets().size() != column.size() + 1) {
    return std::nullopt;
  }

  cudf::strings_column_view scv(column);
  auto const null_count = column.nullable() ? column.null_count() : 0;
  if (column.nullable() && null_count != 0) return std::nullopt;

  std::int64_t const chars_bytes = scv.chars_size(stream);  // the one scalar D2H + sync
  if (chars_bytes > static_cast<std::int64_t>(std::numeric_limits<cudf::size_type>::max())) {
    return std::nullopt;
  }

  auto const u8 = cudf::data_type{cudf::type_id::UINT8};

  staged_root_projection projection;
  projection.channels.reserve(2);
  projection.channels.push_back({{"offsets", scv.offsets()}, root_component::offsets});
  projection.channels.push_back(
    {{"chars",
      cudf::column_view{u8,
                        static_cast<cudf::size_type>(chars_bytes),
                        chars_bytes > 0 ? scv.chars_begin(stream) : nullptr,
                        nullptr,
                        0}},
     root_component::chars});
  return projection;
}

std::unique_ptr<cudf::column> str_split_compressor::decompress(
  compressed_representation const& data_to_decompress,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  auto const* repr = dynamic_cast<str_split_compressed_representation const*>(&data_to_decompress);
  if (repr == nullptr) return nullptr;
  return repr->decompress(stream, mr);
}

}  // namespace simpatico
