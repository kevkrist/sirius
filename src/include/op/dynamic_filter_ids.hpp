/*
 * Copyright 2025, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <cstdint>

namespace sirius::op {

enum class dynamic_filter_publication_outcome : std::uint8_t {
  PUBLISHED,
  NO_MATERIALIZATION,
  FAILED,
  CANCELLED
};

enum class dynamic_filter_no_materialization_reason : std::uint8_t {
  NONE,
  EMPTY_BUILD,
  NO_BUILD_DELIVERY,
  UNSUPPORTED_MODE,
  POLICY_SKIPPED,
  SOURCE_UNAVAILABLE,
  CONSUMER_CLOSED
};

using dynamic_filter_publication_plan_id = std::uint32_t;
using dynamic_filter_target_id           = std::uint32_t;
using dynamic_filter_channel_id          = std::uint32_t;
using dynamic_filter_filter_id           = std::uint32_t;

[[nodiscard]] const char* to_string(dynamic_filter_publication_outcome outcome) noexcept;
[[nodiscard]] const char* to_string(dynamic_filter_no_materialization_reason reason) noexcept;

}  // namespace sirius::op
