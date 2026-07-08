/*
 * Copyright 2025, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "op/sirius_dynamic_filter.hpp"
#include "telemetry/dynamic_filter_telemetry.hpp"

#include <catch.hpp>

TEST_CASE("dynamic-filter ids are monotonic, independent, and reset per query",
          "[dynamic_filter][dynf_telemetry]")
{
  auto& stats = sirius::telemetry::dynamic_filter_query_stats::instance();
  stats.reset_for_testing();
  REQUIRE(stats.next_publication_plan_id() == 1);
  REQUIRE(stats.next_publication_plan_id() == 2);
  REQUIRE(stats.next_target_id() == 1);
  REQUIRE(stats.next_channel_id() == 1);
  REQUIRE(stats.next_filter_id() == 1);
  stats.reset_for_testing();
  REQUIRE(stats.next_publication_plan_id() == 1);
  REQUIRE(stats.next_filter_id() == 1);
}

TEST_CASE("dynamic-filter channels mint ids and close idempotently",
          "[dynamic_filter][dynf_telemetry]")
{
  auto& stats = sirius::telemetry::dynamic_filter_query_stats::instance();
  stats.reset_for_testing();
  sirius::op::sirius_dynamic_filter_set first;
  sirius::op::sirius_dynamic_filter_set second;
  REQUIRE(first.channel_id() != second.channel_id());
  REQUIRE(first.accepting_filters());
  first.close_for_new_filters();
  REQUIRE_FALSE(first.accepting_filters());
  first.close_for_new_filters();
  REQUIRE_FALSE(first.accepting_filters());
}

TEST_CASE("dynamic-filter lifecycle gauges balance", "[dynamic_filter][dynf_telemetry]")
{
  auto& stats = sirius::telemetry::dynamic_filter_query_stats::instance();
  stats.reset_for_testing();
  stats.on_build_batch_pinned();
  stats.on_hash_table_built();
  stats.add_replica_bytes(4096);
  stats.feeder_running_inc();
  auto during = stats.snapshot_for_testing();
  REQUIRE(during.pinned_build_hwm == 1);
  REQUIRE(during.hash_tables_hwm == 1);
  REQUIRE(during.replica_bytes_hwm == 4096);
  REQUIRE(during.feeder_running_hwm == 1);
  stats.on_build_state_released(1, 1);
  stats.sub_replica_bytes(4096);
  stats.feeder_running_dec();
  auto after = stats.snapshot_for_testing();
  REQUIRE(after.pinned_build_end == 0);
  REQUIRE(after.hash_tables_end == 0);
  REQUIRE(after.replica_bytes_end == 0);
  REQUIRE(after.feeder_running == 0);
}
