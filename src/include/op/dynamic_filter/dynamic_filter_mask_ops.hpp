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

#include <cudf/column/column.hpp>
#include <cudf/column/column_view.hpp>
#include <cudf/types.hpp>

#include <rmm/cuda_stream_view.hpp>

#include <optional>

namespace sirius::op {

/**
 * @brief Folds a keep mask into a running conjunction and counts the survivors, in one pass
 *
 * After the enqueued kernel completes, every row of @p accumulator holds
 * `accumulator[i] && mask[i]` where a null in either operand counts as `false`, and
 * `*survivor_count` has been incremented by the number of `true` rows. Without @p mask only the
 * accumulator's own nulls are folded and counted.
 *
 * @p accumulator is a BOOL8 column of the same size as @p mask; its null mask (if any) is folded
 * into the values, so the caller must drop it afterwards (`set_null_mask({}, 0)`) before handing
 * the column to consumers that honour null masks. @p survivor_count is a device-resident counter
 * the caller zeroed on @p stream; several calls may accumulate into distinct counters of one
 * buffer.
 *
 * @throw cucascade::cuda_error if the launch fails
 */
void and_mask_count(cudf::column& accumulator,
                    std::optional<cudf::column_view> mask,
                    cudf::size_type* survivor_count,
                    rmm::cuda_stream_view stream);

}  // namespace sirius::op
