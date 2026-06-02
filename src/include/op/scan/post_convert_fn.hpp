/*
 * Copyright 2025, Sirius Contributors.
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

#include <cudf/table/table.hpp>

#include <rmm/cuda_stream_view.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sirius::op::scan {

/// Post-decode hook applied by the GPU parquet scan operator to a freshly read
/// data batch before output assembly.  Used by iceberg to chain delete-filter
/// stages (positional + equality) onto the new GPU scan path without coupling
/// non-iceberg code to iceberg headers.
///
/// The input table is what the cudf parquet reader produced for this batch:
/// the selected data columns (plus any trailing pure-filter / extra columns the
/// scan_info force-projected) in the parquet schema column order.  The scan
/// operator invokes the hook AFTER any post-read filter expression evaluation
/// and BEFORE @c assemble_scan_output reshapes the table to the operator's
/// output projection.
///
/// @param table       The decoded cudf::table for this batch.
/// @param data_path   Absolute path of the data file this batch came from
///                    (positional-delete lookup keys on this).
/// @param first_row   Absolute row offset of @c table's first row within
///                    @p data_path, computed by the operator from the
///                    surviving row groups of the emitted slice.
/// @param stream      CUDA stream for any GPU work the hook queues.
/// @return Filtered table; may reuse the input buffers when nothing changed.
using scan_post_decode_hook_t =
  std::function<std::unique_ptr<cudf::table>(std::unique_ptr<cudf::table> table,
                                             std::string const& data_path,
                                             int64_t first_row,
                                             rmm::cuda_stream_view stream)>;

}  // namespace sirius::op::scan
