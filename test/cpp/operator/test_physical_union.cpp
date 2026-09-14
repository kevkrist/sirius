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

#include "catch.hpp"
#include "creator/task_creator.hpp"
#include "op/sirius_physical_union.hpp"
#include "operator_test_utils.hpp"
#include "pipeline/sirius_pipeline.hpp"

#include <cudf/types.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace {

using sirius::op::MemoryBarrierType;
using sirius::op::sirius_physical_operator;
using sirius::op::sirius_physical_union;
using sirius::op::SiriusPhysicalOperatorType;
using sirius::op::TaskCreationHint;
using sirius::pipeline::pipeline_build_context;
using sirius::pipeline::sirius_pipeline;
using sirius::pipeline::sirius_pipeline_build_state;

class controllable_pipeline final : public sirius_pipeline {
 public:
  explicit controllable_pipeline(const pipeline_build_context& context) : sirius_pipeline(context)
  {
  }

  void set_finished(bool finished) { _finished.store(finished); }

  bool is_pipeline_finished() const override { return _finished.load(); }

 private:
  std::atomic<bool> _finished{false};
};

class recording_task_creator final : public sirius::creator::task_creator {
 public:
  recording_task_creator(sirius::memory::sirius_memory_reservation_manager& mem_mgr,
                         int num_threads)
    : task_creator(
        sirius::creator::task_creator_config{
          .thread_pool = {.num_threads = num_threads, .thread_name_prefix = "task_creator"}},
        mem_mgr)
  {
  }

  void schedule(sirius_physical_operator* request) override
  {
    std::lock_guard<std::mutex> guard(_mutex);
    _scheduled.push_back(request);
  }

  std::size_t schedule_count(const sirius_physical_operator* request)
  {
    std::lock_guard<std::mutex> guard(_mutex);
    return static_cast<std::size_t>(std::count(_scheduled.begin(), _scheduled.end(), request));
  }

 private:
  std::mutex _mutex;
  std::vector<sirius_physical_operator*> _scheduled;
};

class union_fixture {
 private:
  static pipeline_build_context make_build_context(std::size_t source_window)
  {
    auto params                 = std::make_shared<sirius::operator_params>();
    params->union_source_window = source_window;
    return pipeline_build_context{nullptr, true, 1, std::move(params)};
  }

  std::unique_ptr<sirius::memory::sirius_memory_reservation_manager> _memory_manager;
  recording_task_creator _creator;
  pipeline_build_context _build_context;

 public:
  explicit union_fixture(std::size_t num_arms,
                         std::size_t source_window = 4,
                         int creator_threads       = 5)
    : _memory_manager(sirius::test::operator_utils::initialize_memory_manager()),
      _creator(*_memory_manager, creator_threads),
      _build_context(make_build_context(source_window)),
      union_op({}, 0),
      union_pipeline(duckdb::make_shared_ptr<sirius_pipeline>(_build_context))
  {
    union_op.set_pipeline(union_pipeline);
    union_pipeline->set_task_creator(&_creator);

    sirius_pipeline_build_state build_state;
    for (std::size_t arm = 0; arm < num_arms; ++arm) {
      auto producer = duckdb::make_uniq<sirius_physical_operator>(
        SiriusPhysicalOperatorType::PROJECTION, duckdb::vector<sirius::logical_type>{}, 0);
      auto* producer_ptr = producer.get();
      union_op.children.push_back(std::move(producer));
      producers.push_back(producer_ptr);

      auto pipeline = duckdb::make_shared_ptr<controllable_pipeline>(_build_context);
      duckdb::vector<std::reference_wrapper<sirius_physical_operator>> operators;
      operators.emplace_back(*producer_ptr);
      build_state.set_pipeline_operators(*pipeline, std::move(operators));
      pipeline->set_pipeline_id(arm);
      pipeline->set_task_creator(&_creator);
      source_pipelines.push_back(pipeline);

      auto repository     = std::make_unique<cucascade::shared_data_repository>();
      auto port           = std::make_unique<sirius_physical_operator::port>();
      port->type          = MemoryBarrierType::PARTIAL;
      port->repo          = repository.get();
      port->src_pipeline  = pipeline;
      port->dest_pipeline = union_pipeline;
      union_op.add_port(sirius_physical_union::port_label(arm), std::move(port));
      repositories.push_back(std::move(repository));
    }
  }

  std::shared_ptr<cucascade::data_batch> make_batch(int32_t value)
  {
    auto* gpu_space =
      _memory_manager->get_memory_space(cucascade::memory::Tier::GPU, /*device_id=*/0);
    REQUIRE(gpu_space != nullptr);
    return sirius::test::operator_utils::make_numeric_batch<int32_t>(
      *gpu_space, {value}, cudf::type_id::INT32);
  }

  std::size_t regular_schedule_count(const sirius_physical_operator* request)
  {
    return _creator.schedule_count(request);
  }

  recording_task_creator& task_creator() { return _creator; }

  void use_task_creator(recording_task_creator& creator)
  {
    union_pipeline->set_task_creator(&creator);
    for (auto& pipeline : source_pipelines) {
      pipeline->set_task_creator(&creator);
    }
  }

  sirius_physical_union union_op;
  duckdb::shared_ptr<sirius_pipeline> union_pipeline;
  std::vector<sirius_physical_operator*> producers;
  std::vector<duckdb::shared_ptr<controllable_pipeline>> source_pipelines;
  std::vector<std::unique_ptr<cucascade::shared_data_repository>> repositories;
};

}  // namespace

TEST_CASE("physical_union configured width one admits arms in child order", "[physical_union]")
{
  union_fixture fixture(3, 1);

  auto hint = fixture.union_op.get_next_task_hint();
  REQUIRE(hint.has_value());
  REQUIRE(hint->hint == TaskCreationHint::NOMINATE_PRODUCERS);
  REQUIRE(hint->producer == nullptr);
  REQUIRE(hint->additional_producers ==
          std::vector<sirius_physical_operator*>{fixture.producers[0]});
  REQUIRE_FALSE(fixture.union_op.get_next_task_hint().has_value());

  fixture.source_pipelines[0]->set_finished(true);
  hint = fixture.union_op.get_next_task_hint();
  REQUIRE(hint.has_value());
  REQUIRE(hint->hint == TaskCreationHint::NOMINATE_PRODUCERS);
  REQUIRE(hint->additional_producers ==
          std::vector<sirius_physical_operator*>{fixture.producers[1]});
}

TEST_CASE("physical_union default window admits four arms and refills one slot", "[physical_union]")
{
  union_fixture fixture(6);

  auto hint = fixture.union_op.get_next_task_hint();
  REQUIRE(hint.has_value());
  REQUIRE(hint->hint == TaskCreationHint::NOMINATE_PRODUCERS);
  REQUIRE(
    hint->additional_producers ==
    std::vector<sirius_physical_operator*>{
      fixture.producers[0], fixture.producers[1], fixture.producers[2], fixture.producers[3]});
  REQUIRE_FALSE(fixture.union_op.get_next_task_hint().has_value());

  fixture.source_pipelines[0]->set_finished(true);
  fixture.repositories[0]->add_data_batch(fixture.make_batch(0));
  fixture.source_pipelines[1]->set_finished(true);
  hint = fixture.union_op.get_next_task_hint();
  REQUIRE(hint.has_value());
  REQUIRE(hint->hint == TaskCreationHint::READY);
  REQUIRE(hint->producer == &fixture.union_op);
  REQUIRE(hint->additional_producers ==
          std::vector<sirius_physical_operator*>{fixture.producers[4]});

  REQUIRE(fixture.union_op.get_next_task_input_data() != nullptr);
  REQUIRE(fixture.union_op.take_task_creation_recheck());
  REQUIRE_FALSE(fixture.union_op.take_task_creation_recheck());

  hint = fixture.union_op.get_next_task_hint();
  REQUIRE(hint.has_value());
  REQUIRE(hint->hint == TaskCreationHint::NOMINATE_PRODUCERS);
  REQUIRE(hint->additional_producers ==
          std::vector<sirius_physical_operator*>{fixture.producers[5]});
  REQUIRE(fixture.regular_schedule_count(&fixture.union_op) == 0);
}

TEST_CASE("physical_union adopts preseeded arms before child-order admissions", "[physical_union]")
{
  union_fixture fixture(6, 2);
  fixture.repositories[4]->add_data_batch(fixture.make_batch(4));
  fixture.repositories[5]->add_data_batch(fixture.make_batch(5));
  fixture.source_pipelines[5]->set_finished(true);

  auto hint = fixture.union_op.get_next_task_hint();
  REQUIRE(hint.has_value());
  REQUIRE(hint->hint == TaskCreationHint::READY);
  REQUIRE(hint->producer == &fixture.union_op);
  REQUIRE(hint->additional_producers ==
          std::vector<sirius_physical_operator*>{fixture.producers[4]});

  REQUIRE(fixture.union_op.get_next_task_input_data() != nullptr);
  REQUIRE(fixture.repositories[4]->total_size() == 0);
  REQUIRE(fixture.repositories[5]->total_size() == 1);
  REQUIRE(fixture.union_op.get_next_task_input_data() != nullptr);
  REQUIRE(fixture.repositories[5]->total_size() == 0);
  REQUIRE(fixture.union_op.take_task_creation_recheck());
}

TEST_CASE("physical_union drains admitted arms round-robin", "[physical_union]")
{
  union_fixture fixture(3, 3);
  REQUIRE(fixture.union_op.get_next_task_hint()->additional_producers.size() == 3);

  fixture.repositories[0]->add_data_batch(fixture.make_batch(0));
  fixture.repositories[0]->add_data_batch(fixture.make_batch(10));
  fixture.repositories[1]->add_data_batch(fixture.make_batch(1));
  fixture.repositories[1]->add_data_batch(fixture.make_batch(11));
  fixture.repositories[2]->add_data_batch(fixture.make_batch(2));
  fixture.repositories[2]->add_data_batch(fixture.make_batch(12));

  REQUIRE(fixture.union_op.get_next_task_input_data() != nullptr);
  REQUIRE(fixture.repositories[0]->total_size() == 1);
  REQUIRE(fixture.union_op.get_next_task_input_data() != nullptr);
  REQUIRE(fixture.repositories[1]->total_size() == 1);
  REQUIRE(fixture.union_op.get_next_task_input_data() != nullptr);
  REQUIRE(fixture.repositories[2]->total_size() == 1);
  REQUIRE(fixture.union_op.get_next_task_input_data() != nullptr);
  REQUIRE(fixture.repositories[0]->total_size() == 0);
}

TEST_CASE("physical_union clamps its window to creator capacity and arm count", "[physical_union]")
{
  SECTION("one creator thread")
  {
    union_fixture fixture(3, 4, 1);
    auto hint = fixture.union_op.get_next_task_hint();
    REQUIRE(hint->additional_producers ==
            std::vector<sirius_physical_operator*>{fixture.producers[0]});
  }

  SECTION("fewer arms than the window")
  {
    union_fixture fixture(2);
    auto hint = fixture.union_op.get_next_task_hint();
    REQUIRE(hint->additional_producers ==
            std::vector<sirius_physical_operator*>{fixture.producers[0], fixture.producers[1]});
  }
}

TEST_CASE("concurrent physical unions keep independent bounded source windows", "[physical_union]")
{
  union_fixture left(6, 4, 5);
  union_fixture right(6, 4, 5);
  right.use_task_creator(left.task_creator());

  auto left_future =
    std::async(std::launch::async, [&] { return left.union_op.get_next_task_hint(); });
  auto right_future =
    std::async(std::launch::async, [&] { return right.union_op.get_next_task_hint(); });
  REQUIRE(left_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
  REQUIRE(right_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
  auto left_hint  = left_future.get();
  auto right_hint = right_future.get();
  REQUIRE(left_hint->additional_producers.size() == 4);
  REQUIRE(right_hint->additional_producers.size() == 4);

  left.source_pipelines[0]->set_finished(true);
  right.source_pipelines[0]->set_finished(true);
  left_hint  = left.union_op.get_next_task_hint();
  right_hint = right.union_op.get_next_task_hint();
  REQUIRE(left_hint->additional_producers ==
          std::vector<sirius_physical_operator*>{left.producers[4]});
  REQUIRE(right_hint->additional_producers ==
          std::vector<sirius_physical_operator*>{right.producers[4]});

  left.source_pipelines[1]->set_finished(true);
  right.source_pipelines[1]->set_finished(true);
  left.repositories[1]->add_data_batch(left.make_batch(1));
  right.repositories[1]->add_data_batch(right.make_batch(2));
  REQUIRE(left.union_op.get_next_task_hint()->hint == TaskCreationHint::READY);
  REQUIRE(right.union_op.get_next_task_hint()->hint == TaskCreationHint::READY);
  REQUIRE(left.union_op.get_next_task_input_data() != nullptr);
  REQUIRE(right.union_op.get_next_task_input_data() != nullptr);
}

TEST_CASE("physical_union skips zero-output arms while filling its window", "[physical_union]")
{
  union_fixture fixture(6);
  for (std::size_t arm = 0; arm < 4; ++arm) {
    fixture.source_pipelines[arm]->set_finished(true);
  }

  auto hint = fixture.union_op.get_next_task_hint();
  REQUIRE(hint.has_value());
  REQUIRE(hint->hint == TaskCreationHint::NOMINATE_PRODUCERS);
  REQUIRE(hint->additional_producers ==
          std::vector<sirius_physical_operator*>{fixture.producers[4], fixture.producers[5]});
}

TEST_CASE("physical_union does not request a recheck after its final arm", "[physical_union]")
{
  union_fixture fixture(1);
  fixture.source_pipelines[0]->set_finished(true);
  fixture.repositories[0]->add_data_batch(fixture.make_batch(0));

  auto hint = fixture.union_op.get_next_task_hint();
  REQUIRE(hint->hint == TaskCreationHint::READY);
  REQUIRE(hint->additional_producers.empty());
  REQUIRE(fixture.union_op.get_next_task_input_data() != nullptr);
  REQUIRE_FALSE(fixture.union_op.take_task_creation_recheck());
}
