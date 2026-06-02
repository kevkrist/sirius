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

#include "op/scan/iceberg_metadata_reader.hpp"
#include "op/scan/parquet_scan_info.hpp"

#include <cstddef>
#include <memory>

namespace sirius::op::scan {

/**
 * @brief Bind-data for an iceberg scan.
 *
 * Carries everything @ref parquet_scan_info does, plus the pre-materialized
 * delete data for this Iceberg table snapshot.  Built by the pipeline converter
 * from the engine-level @c iceberg_delete_data_cache_ (populated once by
 * @c sirius_engine::prefetch_iceberg_delete_data).  @ref make_provider builds
 * an @c iceberg_split_provider, which mirrors @c parquet_split_provider's
 * row-group emission but attaches a delete-filter hook to each split.
 */
struct iceberg_scan_info : parquet_scan_info {
  /// Shared, immutable delete metadata for this table snapshot.  May be null
  /// when the cache lookup found no entry (treated as V1: no deletes).
  std::shared_ptr<const IcebergDeleteData> delete_data;

  std::unique_ptr<scan_manager::split_provider> make_provider(
    scan_manager::sirius_scan_manager& manager,
    std::unordered_map<int, std::shared_ptr<sirius::io::sirius_ioctx>> const& gpu_ioctxs) override;
};

}  // namespace sirius::op::scan
