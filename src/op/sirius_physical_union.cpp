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

#include "op/sirius_physical_union.hpp"

#include "op/sirius_physical_passthrough_sink.hpp"
#include "pipeline/sirius_meta_pipeline.hpp"
#include "pipeline/sirius_pipeline.hpp"
#include "sirius/exception.hpp"

#include <nvtx3/nvtx3.hpp>

#include <algorithm>
#include <utility>

namespace sirius {
namespace op {

sirius_physical_union::sirius_physical_union(duckdb::vector<sirius::logical_type> types,
                                             std::size_t estimated_cardinality)
  : sirius_physical_operator(
      SiriusPhysicalOperatorType::UNION, std::move(types), estimated_cardinality)
{
}

std::string sirius_physical_union::get_name() const { return "UNION"; }

bool sirius_physical_union::is_source() const { return true; }

sirius::OrderPreservationType sirius_physical_union::source_order() const
{
  return sirius::OrderPreservationType::NO_ORDER;
}

void sirius_physical_union::build_pipelines(pipeline::sirius_pipeline& current,
                                            pipeline::sirius_meta_pipeline& meta_pipeline)
{
  // Mirrors sirius_physical_hash_join::build_pipelines, generalized from two sides to N arms.
  pipeline::sirius_meta_pipeline* host_meta;
  pipeline::sirius_pipeline* host_current;
  if (is_sink()) {
    auto& sink_meta = meta_pipeline.create_child_meta_pipeline(current, *this);
    host_meta       = &sink_meta;
    host_current    = sink_meta.get_base_pipeline().get();
  } else {
    meta_pipeline.get_state().add_pipeline_operator(current, *this);
    host_meta    = &meta_pipeline;
    host_current = &current;
  }

  // Every arm reaches UNION through a plan-gen PASSTHROUGH_SINK wrap. Create a child meta per arm
  // terminating in that sink, then recurse *past* it so it does not redundantly create its own.
  // A throw rather than a D_ASSERT, because a release build would otherwise index an empty
  // `children` and read past the end silently. The other two preconditions already fail loudly
  // elsewhere: arity in the plan builder (`sirius_plan_set_operation.cpp`), and a non-sink
  // pipeline sink in `sirius_pipeline::reset_sink`.
  for (auto& child_slot : children) {
    auto& child = *child_slot;
    if (child.children.empty()) {
      throw internal_exception(
        "sirius_physical_union::build_pipelines: arm reached pipeline building without its "
        "PASSTHROUGH_SINK wrap");
    }
    auto& child_meta = host_meta->create_child_meta_pipeline(*host_current, child);
    child_meta.build(*child.children[0]);
  }
}

duckdb::vector<duckdb::const_reference<sirius_physical_operator>>
sirius_physical_union::get_sources() const
{
  duckdb::vector<duckdb::const_reference<sirius_physical_operator>> result;
  if (is_sink()) {
    result.push_back(*this);
    return result;
  }
  for (const auto& child : children) {
    auto child_sources = child->get_sources();
    for (const auto& source : child_sources) {
      result.push_back(source);
    }
  }
  return result;
}

std::unique_ptr<operator_data> sirius_physical_union::execute(const operator_data& input_data,
                                                              rmm::cuda_stream_view /*stream*/)
{
  nvtx3::scoped_range nvtx_range{"sirius_physical_union::execute"};
  // get_next_task_input_data already popped the batch; re-wrap it. Forwarding the read-only
  // accessors keeps the shared read lock held across the handoff.
  const auto* pipelineable = dynamic_cast<const pipelineable_operator_data*>(&input_data);
  if (pipelineable == nullptr) {
    throw internal_exception("sirius_physical_union::execute: expected pipelineable_operator_data");
  }
  return std::make_unique<pipelineable_operator_data>(pipelineable->get_read_only_batches(false));
}

const std::vector<sirius_physical_operator::port*>& sirius_physical_union::arm_ports()
{
  if (_arm_ports.size() == children.size()) { return _arm_ports; }
  _arm_ports.clear();
  _arm_ports.reserve(children.size());
  for (std::size_t i = 0; i < children.size(); i++) {
    // get_port throws when an arm has no port, meaning the wiring dropped that arm. Failing
    // loudly is the point: the alternative is silently returning a short row count.
    _arm_ports.push_back(get_port(port_label(i)));
  }
  return _arm_ports;
}

void sirius_physical_union::initialize_arm_states()
{
  if (_arm_states.size() == children.size()) { return; }

  _arm_states.assign(children.size(), arm_state::dormant);
  _next_admission_cursor = 0;
  _drain_cursor          = 0;
  _window_occupancy      = 0;

  auto pipeline           = get_pipeline();
  auto configured_window  = pipeline ? pipeline->get_operator_params().union_source_window : 1;
  configured_window       = std::max<std::size_t>(configured_window, 1);
  auto per_union_capacity = pipeline ? pipeline->per_union_source_window_capacity() : 1;
  _effective_window       = std::min({configured_window, per_union_capacity, children.size()});
}

void sirius_physical_union::refresh_admitted_arms(bool& slot_opened)
{
  const auto& ports_by_arm = arm_ports();
  for (std::size_t arm = 0; arm < _arm_states.size(); ++arm) {
    auto& state = _arm_states[arm];
    auto* p     = ports_by_arm[arm];
    if (state == arm_state::nominated && p->src_pipeline &&
        p->src_pipeline->is_pipeline_finished()) {
      state = arm_state::finished;
    }
    if (state == arm_state::finished && (!p->repo || p->repo->total_size() == 0)) {
      state = arm_state::drained;
      --_window_occupancy;
      slot_opened = true;
    }
  }
}

void sirius_physical_union::admit_arm(std::size_t arm_index,
                                      std::vector<sirius_physical_operator*>& producer_nominations)
{
  auto* p = arm_ports()[arm_index];
  if (!p->src_pipeline) {
    throw internal_exception("sirius_physical_union::admit_arm: arm port has no source pipeline");
  }
  if (!p->repo) {
    throw internal_exception("sirius_physical_union::admit_arm: arm port has no repository");
  }

  if (p->src_pipeline->is_pipeline_finished()) {
    if (p->repo->total_size() > 0) {
      _arm_states[arm_index] = arm_state::finished;
      ++_window_occupancy;
    } else {
      _arm_states[arm_index] = arm_state::drained;
    }
    return;
  }

  auto producers = p->src_pipeline->get_operators();
  if (producers.empty()) {
    throw internal_exception(
      "sirius_physical_union::admit_arm: source pipeline has no producer operator");
  }
  _arm_states[arm_index] = arm_state::nominated;
  ++_window_occupancy;
  producer_nominations.push_back(&producers.front().get());
}

bool sirius_physical_union::has_dormant_arm() const
{
  return std::find(_arm_states.begin(), _arm_states.end(), arm_state::dormant) != _arm_states.end();
}

std::string_view sirius_physical_union::input_port_for(
  sirius_physical_operator const& producer) const
{
  if (producer.type == SiriusPhysicalOperatorType::PASSTHROUGH_SINK) {
    return producer.Cast<sirius_physical_passthrough_sink>().union_port_label();
  }
  return sirius_physical_operator::input_port_for(producer);
}

MemoryBarrierType sirius_physical_union::input_barrier_for(
  sirius_physical_operator const& producer) const
{
  return producer.type == SiriusPhysicalOperatorType::PASSTHROUGH_SINK
           ? MemoryBarrierType::PARTIAL
           : sirius_physical_operator::input_barrier_for(producer);
}

std::optional<task_creation_hint> sirius_physical_union::get_next_task_hint()
{
  std::unique_lock<std::mutex> lg(lock);
  _task_creation_recheck = false;

  const auto& ports_by_arm = arm_ports();
  initialize_arm_states();

  bool slot_opened = false;
  refresh_admitted_arms(slot_opened);

  std::vector<sirius_physical_operator*> producer_nominations;
  for (std::size_t arm = 0; arm < _arm_states.size() && _window_occupancy < _effective_window;
       ++arm) {
    auto* p = ports_by_arm[arm];
    if (_arm_states[arm] == arm_state::dormant && p->repo && p->repo->total_size() > 0) {
      admit_arm(arm, producer_nominations);
    }
  }

  while (_window_occupancy < _effective_window && _next_admission_cursor < _arm_states.size()) {
    const auto arm = _next_admission_cursor++;
    if (_arm_states[arm] == arm_state::dormant) { admit_arm(arm, producer_nominations); }
  }

  bool has_ready_arm = false;
  for (std::size_t arm = 0; arm < _arm_states.size(); ++arm) {
    const auto state = _arm_states[arm];
    auto* p          = ports_by_arm[arm];
    if ((state == arm_state::nominated || state == arm_state::finished) && p->repo &&
        p->repo->total_size() > 0) {
      has_ready_arm = true;
      break;
    }
  }
  if (has_ready_arm) {
    return task_creation_hint{TaskCreationHint::READY, this, std::move(producer_nominations)};
  }
  if (!producer_nominations.empty()) {
    return task_creation_hint{
      TaskCreationHint::NOMINATE_PRODUCERS, nullptr, std::move(producer_nominations)};
  }

  return std::nullopt;
}

std::unique_ptr<operator_data> sirius_physical_union::get_next_task_input_data()
{
  std::unique_lock<std::mutex> lg(lock);

  const auto& ports_by_arm = arm_ports();
  initialize_arm_states();

  bool slot_opened = false;
  refresh_admitted_arms(slot_opened);

  const auto arm_count = _arm_states.size();
  for (std::size_t offset = 0; offset < arm_count; ++offset) {
    const auto arm = (_drain_cursor + offset) % arm_count;
    auto state     = _arm_states[arm];
    if (state != arm_state::nominated && state != arm_state::finished) { continue; }

    auto* p = ports_by_arm[arm];
    if (!p->repo) { continue; }
    auto batch = p->repo->pop_next_data_batch();
    if (!batch) { continue; }

    _drain_cursor = (arm + 1) % arm_count;
    if (p->src_pipeline && p->src_pipeline->is_pipeline_finished()) {
      _arm_states[arm] = arm_state::finished;
    }
    if (_arm_states[arm] == arm_state::finished && p->repo->total_size() == 0) {
      _arm_states[arm] = arm_state::drained;
      --_window_occupancy;
      slot_opened = true;
    }
    if (slot_opened && _window_occupancy < _effective_window && has_dormant_arm()) {
      _task_creation_recheck = true;
    }

    std::vector<std::shared_ptr<::cucascade::data_batch>> popped;
    popped.push_back(std::move(batch));
    return std::make_unique<pipelineable_operator_data>(std::move(popped));
  }

  if (slot_opened && _window_occupancy < _effective_window && has_dormant_arm()) {
    _task_creation_recheck = true;
  }
  return nullptr;
}

bool sirius_physical_union::take_task_creation_recheck()
{
  std::lock_guard<std::mutex> lg(lock);
  return std::exchange(_task_creation_recheck, false);
}

}  // namespace op
}  // namespace sirius
