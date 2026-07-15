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

// sirius
#include <compression/compressed_representation.hpp>
#include <cucascade/cudf/gpu_data_representation.hpp>
#include <data/sirius_converter_registry.hpp>
#include <op/scan/sirius_gpu_scan_operator_data.hpp>

#include <limits>
#include <stdexcept>

namespace sirius::op::scan {

void scan_operator_input::prepare_for_processing(
  const ::cucascade::memory::memory_space* requested_memory_space, rmm::cuda_stream_view stream)
{
  auto* conversion_target = const_cast<::cucascade::memory::memory_space*>(requested_memory_space);
  gpu_memory_space        = conversion_target;
  if (!std::holds_alternative<std::shared_ptr<cucascade::data_batch>>(materialization_info)) {
    prefetch(io::cache::prefetching_stage::just_in_time);
    return;
  }

  auto const& batch = std::get<std::shared_ptr<cucascade::data_batch>>(materialization_info);
  if (!batch) {
    throw std::invalid_argument("[scan_operator_input] cached batch must not be null");
  }

  bool needs_upload = false;
  {
    auto ro                  = batch->to_read_only();
    auto const* data         = ro.get_data();
    auto* const source_space = ro.get_memory_space();
    if (data == nullptr) {
      throw std::invalid_argument(
        "[scan_operator_input] cached batch must have a data representation");
    }

    // Convert when data is not already a plain GPU table in the requested space.
    // A compressed_device_representation needs in-place decompression, while a
    // plain table in another GPU space uses cuCascade's registered GPU-to-GPU
    // converter.
    auto const is_gpu_table =
      dynamic_cast<const ::cucascade::gpu_table_representation*>(data) != nullptr;
    auto const source_is_gpu =
      source_space != nullptr && source_space->get_tier() == ::cucascade::memory::Tier::GPU;
    auto const target_differs =
      conversion_target != nullptr &&
      (source_space == nullptr || source_space->get_id() != conversion_target->get_id());
    needs_upload = !source_is_gpu || !is_gpu_table || target_differs;

    // A compressed device representation can safely materialize on its current
    // GPU when the scheduler supplied no explicit target.
    if (conversion_target == nullptr && source_is_gpu) { conversion_target = source_space; }
  }

  if (needs_upload) {
    if (conversion_target == nullptr ||
        conversion_target->get_tier() != ::cucascade::memory::Tier::GPU) {
      throw std::invalid_argument(
        "[scan_operator_input] materializing cached data requires a target GPU memory space");
    }
    auto& registry = ::sirius::converter_registry::get();
    auto mut       = batch->to_mutable();
    mut.convert_to<::cucascade::gpu_table_representation>(registry, conversion_target, stream);
  }

  auto ro          = batch->to_read_only();
  gpu_memory_space = ro.get_memory_space();
}

std::size_t scan_operator_input::get_estimated_size_in_bytes() const
{
  if (std::holds_alternative<std::unique_ptr<scan_info>>(materialization_info)) {
    auto const& metadata = std::get<std::unique_ptr<scan_info>>(materialization_info);
    return metadata ? metadata->estimated_bytes() : 0;
  }
  if (std::holds_alternative<std::shared_ptr<cucascade::data_batch>>(materialization_info)) {
    auto const& batch = std::get<std::shared_ptr<cucascade::data_batch>>(materialization_info);
    if (!batch) { return 0; }

    auto ro          = batch->to_read_only();
    auto const* data = ro.get_data();
    return data ? data->get_uncompressed_data_size_in_bytes() : 0;
  }
  return 0;
}

std::size_t scan_operator_input::get_estimated_working_set_size_in_bytes() const
{
  if (std::holds_alternative<std::unique_ptr<scan_info>>(materialization_info)) {
    auto const& metadata = std::get<std::unique_ptr<scan_info>>(materialization_info);
    return metadata ? metadata->estimated_working_set_bytes() : 0;
  }
  if (std::holds_alternative<std::shared_ptr<cucascade::data_batch>>(materialization_info)) {
    auto const& batch = std::get<std::shared_ptr<cucascade::data_batch>>(materialization_info);
    if (!batch) { return 0; }

    auto ro          = batch->to_read_only();
    auto const* data = ro.get_data();
    if (!data) { return 0; }

    auto const resident_bytes = data->get_size_in_bytes();
    auto const logical_bytes  = data->get_uncompressed_data_size_in_bytes();

    auto const is_compressed =
      dynamic_cast<const ::sirius::compressed_host_representation*>(data) != nullptr ||
      dynamic_cast<const ::sirius::compressed_device_representation*>(data) != nullptr;
    // Reconstructing compressed input creates encoded GPU buffers before the logical table.
    // Raw HOST input and an already-materialized GPU table need only the logical target footprint.
    if (!is_compressed) { return logical_bytes; }
    if (resident_bytes > std::numeric_limits<std::size_t>::max() - logical_bytes) {
      return std::numeric_limits<std::size_t>::max();
    }
    return resident_bytes + logical_bytes;
  }
  return 0;
}

}  // namespace sirius::op::scan
