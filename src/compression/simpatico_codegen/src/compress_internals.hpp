// SPDX-License-Identifier: Apache-2.0
// Internal helpers shared by the staged owning-compress driver (staged_compression.cpp).
// Not part of the installed public headers — do not include from outside src/.
//
// The ordinary compress/decompress entry points keep their own file-local copies of these
// helpers in simpatico_codegen.cpp; this header exists because staged_compression.cpp is a
// separate translation unit that needs the same plan-parsing/validation primitives plus the
// staged-decision completion probe (which the ordinary path has no use for).
#pragma once

#include <rmm/cuda_stream_view.hpp>

#include <cuda_runtime.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace simpatico::detail {

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------

class plan_error : public std::runtime_error {
 public:
  explicit plan_error(std::string const& msg) : std::runtime_error(msg) {}
};

// ---------------------------------------------------------------------------
// Plan DSL splitting
// ---------------------------------------------------------------------------

inline std::string trim_plan_block(std::string s)
{
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
    s.pop_back();
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
    ++start;
  return s.substr(start);
}

// Split a multi-column DSL string on "---" separators, skipping blank lines
// and comment lines (beginning with '#'). Each returned block is trimmed.
inline std::vector<std::string> split_plan_dsl_impl(std::string_view plan_dsl)
{
  std::vector<std::string> plans;
  std::string current;
  size_t i = 0;
  while (i < plan_dsl.size()) {
    size_t line_end = plan_dsl.find('\n', i);
    if (line_end == std::string_view::npos) line_end = plan_dsl.size();
    std::string_view line = plan_dsl.substr(i, line_end - i);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    std::string_view trimmed = line;
    while (!trimmed.empty() && trimmed.front() == ' ')
      trimmed.remove_prefix(1);
    while (!trimmed.empty() && trimmed.back() == ' ')
      trimmed.remove_suffix(1);

    if (trimmed == "---") {
      auto block = trim_plan_block(current);
      if (!block.empty()) plans.push_back(std::move(block));
      current.clear();
    } else if (!trimmed.empty() && trimmed.front() != '#') {
      current.append(trimmed);
      current.push_back('\n');
    }
    i = (line_end == plan_dsl.size()) ? plan_dsl.size() : line_end + 1;
  }
  auto block = trim_plan_block(current);
  if (!block.empty()) plans.push_back(std::move(block));
  return plans;
}

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

inline void validate_plan_count(size_t plan_count, int table_columns)
{
  if (plan_count != static_cast<size_t>(table_columns)) {
    throw plan_error("plan count (" + std::to_string(plan_count) +
                     ") does not match table.num_columns() (" + std::to_string(table_columns) +
                     ")");
  }
}

inline void validate_column_names(std::vector<std::string> const& column_names, size_t num_columns)
{
  if (!column_names.empty() && column_names.size() != num_columns) {
    throw plan_error("column_names size (" + std::to_string(column_names.size()) +
                     ") does not match num_columns (" + std::to_string(num_columns) + ")");
  }
}

// ---------------------------------------------------------------------------
// Staged-decision completion probe
// ---------------------------------------------------------------------------

/// How staged_compression proves its stream is quiescent before accept()/reject() move any
/// ownership. Null in production, where the probe is cudaStreamSynchronize; tests point it at a
/// failing stub to exercise the decision-failure contract without wedging the CUDA context.
using completion_probe = cudaError_t (*)(rmm::cuda_stream_view) noexcept;
extern thread_local completion_probe stage_completion_probe;

}  // namespace simpatico::detail
