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

// Unit tests for the pure, plan-independent core of the date-DIP pass: extracting a
// date window from a pushed-down filter, applying a measured correlation lag, and
// building the prune-only OptionalFilter. The plan-walk (apply_date_dips) needs a bound
// plan + catalog, so it is exercised end-to-end via the SQL/integration harness on a
// clustered dataset rather than here.

#include <catch.hpp>
#include <duckdb/common/enums/expression_type.hpp>
#include <duckdb/common/types/date.hpp>
#include <duckdb/common/types/value.hpp>
#include <duckdb/planner/filter/conjunction_filter.hpp>
#include <duckdb/planner/filter/constant_filter.hpp>
#include <duckdb/planner/filter/optional_filter.hpp>
#include <duckdb/planner/table_filter.hpp>
#include <transparent/sirius_date_dip.hpp>

using namespace duckdb;
using namespace sirius::transparent;

namespace {

// A two-sided orders.o_orderdate range as DuckDB's FILTER_PUSHDOWN would build it:
// a ConjunctionAndFilter of (>= d1) and (< d2).
unique_ptr<TableFilter> orderdate_range(const std::string& d1, const std::string& d2)
{
  auto conj = make_uniq<ConjunctionAndFilter>();
  conj->child_filters.push_back(make_uniq<ConstantFilter>(
    ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::DATE(Date::FromString(d1))));
  conj->child_filters.push_back(
    make_uniq<ConstantFilter>(ExpressionType::COMPARE_LESSTHAN, Value::DATE(Date::FromString(d2))));
  return conj;
}

int32_t days(const std::string& d) { return Date::FromString(d).days; }

const date_correlation kQ5Correlation{
  "orders", "o_orderdate", "o_orderkey", "lineitem", "l_shipdate", "l_orderkey", 0, 121};

}  // namespace

TEST_CASE("extract_date_window reads a two-sided range from a conjunction", "[date_dip]")
{
  auto f = orderdate_range("1997-01-01", "1998-01-01");
  auto w = extract_date_window(*f);
  REQUIRE(w.lo.has_value());
  REQUIRE(w.hi.has_value());
  REQUIRE(w.lo->days == days("1997-01-01"));
  REQUIRE(w.hi->days == days("1998-01-01"));
}

TEST_CASE("extract_date_window unwraps an OptionalFilter", "[date_dip]")
{
  auto inner = orderdate_range("1994-03-01", "1994-06-01");
  auto opt   = make_uniq<OptionalFilter>(std::move(inner));
  auto w     = extract_date_window(*opt);
  REQUIRE(w.lo->days == days("1994-03-01"));
  REQUIRE(w.hi->days == days("1994-06-01"));
}

TEST_CASE("extract_date_window pins both bounds for equality", "[date_dip]")
{
  ConstantFilter eq(ExpressionType::COMPARE_EQUAL, Value::DATE(Date::FromString("1995-06-15")));
  auto w = extract_date_window(eq);
  REQUIRE(w.lo->days == days("1995-06-15"));
  REQUIRE(w.hi->days == days("1995-06-15"));
}

TEST_CASE("extract_date_window ignores a non-DATE constant filter", "[date_dip]")
{
  // A filter on a non-date column must not be misread as a date bound.
  ConstantFilter int_filter(ExpressionType::COMPARE_LESSTHAN, Value::INTEGER(42));
  REQUIRE(extract_date_window(int_filter).empty());
}

TEST_CASE("derive_forward_window applies the dbgen lag (lower 0, upper +121)", "[date_dip]")
{
  derived_date_window dim;
  dim.lo = Date::FromString("1997-01-01");
  dim.hi = Date::FromString("1998-01-01");

  auto out = derive_forward_window(dim, kQ5Correlation);
  REQUIRE(out.lo->days == days("1997-01-01"));        // +0
  REQUIRE(out.hi->days == days("1998-01-01") + 121);  // +121
}

TEST_CASE("derive_forward_window uses only the query-overlapping buckets (non-stationary lag)",
          "[date_dip]")
{
  // A lag histogram with two eras: 1993 ships in [1,10] days, 1997 in [300,360] days.
  date_correlation c = kQ5Correlation;
  c.buckets          = {{days("1993-01-01"), days("1993-12-31"), 1, 10},
                        {days("1997-01-01"), days("1997-12-31"), 300, 360}};

  derived_date_window q1993;
  q1993.lo = Date::FromString("1993-02-01");
  q1993.hi = Date::FromString("1993-03-01");
  auto f1  = derive_forward_window(q1993, c);
  REQUIRE(f1.lo->days == days("1993-02-01") + 1);  // era-1 lag, not the global [.,360]
  REQUIRE(f1.hi->days == days("1993-03-01") + 10);

  derived_date_window q1997;
  q1997.lo = Date::FromString("1997-06-01");
  q1997.hi = Date::FromString("1997-07-01");
  auto f2  = derive_forward_window(q1997, c);
  REQUIRE(f2.lo->days == days("1997-06-01") + 300);  // era-2 lag
  REQUIRE(f2.hi->days == days("1997-07-01") + 360);
}

TEST_CASE("derive_forward_window falls back to the global lag (no/one-sided buckets)", "[date_dip]")
{
  date_correlation c = kQ5Correlation;  // lag [0,121], no buckets
  derived_date_window two_sided;
  two_sided.lo = Date::FromString("1997-01-01");
  two_sided.hi = Date::FromString("1998-01-01");
  auto g       = derive_forward_window(two_sided, c);
  REQUIRE(g.lo->days == days("1997-01-01") + 0);
  REQUIRE(g.hi->days == days("1998-01-01") + 121);

  // A bucketed correlation but a ONE-SIDED window -> global lag (buckets can't be intersected).
  c.buckets = {{days("1997-01-01"), days("1997-12-31"), 300, 360}};
  derived_date_window upper_only;
  upper_only.hi = Date::FromString("1997-07-01");
  auto u        = derive_forward_window(upper_only, c);
  REQUIRE(!u.lo.has_value());
  REQUIRE(u.hi->days == days("1997-07-01") + 121);  // global hi, not the bucket's 360
}

TEST_CASE("make_prune_only_date_filter wraps a two-sided range in an OptionalFilter", "[date_dip]")
{
  derived_date_window w;
  w.lo = Date::FromString("1997-01-01");
  w.hi = Date::FromString("1998-05-02");

  auto f = make_prune_only_date_filter(w);
  REQUIRE(f != nullptr);
  // Top-level OPTIONAL_FILTER => skipped by convert_table_filters_to_expression
  // (row-level no-op) yet honored by mark_row_groups_pruned_by_filter_stats.
  REQUIRE(f->filter_type == TableFilterType::OPTIONAL_FILTER);

  auto& opt = f->Cast<OptionalFilter>();
  REQUIRE(opt.child_filter != nullptr);
  REQUIRE(opt.child_filter->filter_type == TableFilterType::CONJUNCTION_AND);
  REQUIRE(opt.child_filter->Cast<ConjunctionAndFilter>().child_filters.size() == 2);

  // Round-trip: re-extracting the built filter reproduces the window.
  auto rt = extract_date_window(*f);
  REQUIRE(rt.lo->days == w.lo->days);
  REQUIRE(rt.hi->days == w.hi->days);
}

TEST_CASE("make_prune_only_date_filter handles one-sided windows", "[date_dip]")
{
  derived_date_window upper_only;
  upper_only.hi = Date::FromString("1995-07-24");
  auto f        = make_prune_only_date_filter(upper_only);
  REQUIRE(f != nullptr);
  REQUIRE(f->filter_type == TableFilterType::OPTIONAL_FILTER);
  // A single bound is a bare ConstantFilter under the OptionalFilter.
  REQUIRE(f->Cast<OptionalFilter>().child_filter->filter_type ==
          TableFilterType::CONSTANT_COMPARISON);
}

TEST_CASE("make_prune_only_date_filter rejects empty and degenerate windows", "[date_dip]")
{
  REQUIRE(make_prune_only_date_filter(derived_date_window{}) == nullptr);

  derived_date_window degenerate;
  degenerate.lo = Date::FromString("1998-01-01");
  degenerate.hi = Date::FromString("1997-01-01");  // lo > hi
  REQUIRE(make_prune_only_date_filter(degenerate) == nullptr);
}

TEST_CASE("end-to-end derivation reproduces the Q5 lineitem window from a measured lag",
          "[date_dip]")
{
  // A correlation measured as [1, 121] days (as CALL sirius_measure_date_correlation
  // returns for TPC-H) applied to orders.o_orderdate in ['1997-01-01','1998-01-01')
  // -> lineitem.l_shipdate in ['1997-01-01'+1, '1998-01-01'+121].
  const date_correlation measured{
    "orders", "o_orderdate", "o_orderkey", "lineitem", "l_shipdate", "l_orderkey", 1, 121};
  auto orders = orderdate_range("1997-01-01", "1998-01-01");
  auto dim    = extract_date_window(*orders);
  auto fact   = derive_forward_window(dim, measured);
  auto filter = make_prune_only_date_filter(fact);

  REQUIRE(filter != nullptr);
  REQUIRE(filter->filter_type == TableFilterType::OPTIONAL_FILTER);

  auto w = extract_date_window(*filter);
  REQUIRE(w.lo->days == days("1997-01-01") + 1);
  REQUIRE(w.hi->days == days("1998-01-01") + 121);
}
