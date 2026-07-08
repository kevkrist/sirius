/*
 * Copyright 2026, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "catch.hpp"
#include "sirius_config.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

struct temporary_yaml {
  explicit temporary_yaml(std::string_view contents)
    : path(std::filesystem::temp_directory_path() / "sirius_dynamic_filter_priority.yaml")
  {
    std::ofstream out(path);
    out << contents;
    REQUIRE(out);
  }

  ~temporary_yaml()
  {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  std::filesystem::path path;
};

}  // namespace

TEST_CASE("dynamic-filter build priority config defaults to off",
          "[config][dynamic_filter][priority]")
{
  CHECK(sirius::operator_params{}.dynamic_filter_build_priority ==
        sirius::dynamic_filter_build_priority_mode::OFF);
}

TEST_CASE("dynamic-filter build priority string helpers round-trip",
          "[config][dynamic_filter][priority]")
{
  auto mode = sirius::dynamic_filter_build_priority_mode::LEGACY;
  REQUIRE(sirius::string_to_enum("off", mode));
  CHECK(mode == sirius::dynamic_filter_build_priority_mode::OFF);

  std::string out;
  REQUIRE(sirius::enum_to_string(mode, out));
  CHECK(out == "off");
  CHECK_FALSE(sirius::string_to_enum("disabled", mode));
  CHECK_FALSE(sirius::string_to_enum("OFF", mode));
}

TEST_CASE("sirius_config loads dynamic-filter build priority from YAML",
          "[config][dynamic_filter][priority]")
{
  temporary_yaml yaml{
    "sirius:\n"
    "  operator_params:\n"
    "    dynamic_filter_build_priority: off\n"};
  sirius::sirius_config config;
  config.load_from_file(yaml.path);
  CHECK(config.get_operator_params().dynamic_filter_build_priority ==
        sirius::dynamic_filter_build_priority_mode::OFF);
}

TEST_CASE("sirius_config rejects unknown dynamic-filter build priority",
          "[config][dynamic_filter][priority]")
{
  temporary_yaml yaml{
    "sirius:\n"
    "  operator_params:\n"
    "    dynamic_filter_build_priority: fastest\n"};
  sirius::sirius_config config;
  CHECK_THROWS(config.load_from_file(yaml.path));
}
