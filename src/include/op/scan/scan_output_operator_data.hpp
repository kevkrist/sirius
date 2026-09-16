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

#include <op/scan/dynamic_filter_merge.hpp>
#include <op/sirius_physical_operator.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace sirius::op::scan {

/**
 * @brief GPU_SCAN output that records which dynamic filters the scan already applied
 *
 * A scan that folds membership masks into its survivor gather emits this instead of a plain
 * pipelineable payload; the downstream DYNAMIC_FILTER operator reads the tag and only applies
 * filters published after the scan's snapshot. Behaves as a pipelineable payload everywhere else
 * (same type tag), so no other operator needs to know about it.
 */
class scan_output_operator_data final : public pipelineable_operator_data {
 public:
  scan_output_operator_data(std::vector<std::shared_ptr<::cucascade::data_batch>> data_batches,
                            scan_dynamic_filter_result dynamic_filters_applied)
    : pipelineable_operator_data(std::move(data_batches)),
      _dynamic_filters_applied(std::move(dynamic_filters_applied))
  {
  }

  [[nodiscard]] scan_dynamic_filter_result const& dynamic_filters_applied() const noexcept
  {
    return _dynamic_filters_applied;
  }

 private:
  scan_dynamic_filter_result _dynamic_filters_applied;
};

}  // namespace sirius::op::scan
