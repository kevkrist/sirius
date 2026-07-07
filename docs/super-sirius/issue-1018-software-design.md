# Issue #1018 Software Design: Distribution-Aware Exchange

**Status:** Proposed implementation design<br>
**Issue:** [#1018 - Explore avoiding shuffles with pre-partitioned data](https://github.com/sirius-db/sirius/issues/1018)<br>
**Architecture:** [Shuffle Elision with Proven Distribution](issue-1018-prepartitioned-shuffle-design.md)<br>
**Design reference:** [C++ Software Design](https://www.oreilly.com/library/view/c-software-design/9781098113155/titlepage01.html), Klaus Iglberger. Citations of the form GL n refer to its 39 numbered guidelines.

## Purpose

This document turns the architecture decision into an implementation design for Super
Sirius. It deliberately contains class-level contracts, ownership, state transitions,
and a staged change map. The companion architecture document remains the source of
truth for scope, correctness rules, rollout order, and product-level tradeoffs.

The core decision is:

> Distribution is a semantic proof carried by the plan, query, and batch pipeline. It
> is not an integer scheduling hint and it is not inferred from where a batch happens
> to reside.

The first production target remains an INNER hash join followed by a compatible
grouped aggregate. Sirius should reuse the join's established partitioning at the
aggregate exchange. Sirius-created bucketed pinned layouts follow after the
intra-query path proves the contracts.

## Design Guidance Applied

The local PDF recommends managing dependencies and designing explicitly for likely
change. The following guidance materially shapes this design.

| Guidance from the book | Consequence here |
|---|---|
| Design for change and separate concerns (GL 1, GL 2) | Semantic distribution, plan derivation, exchange policy, GPU mechanism, and source adaptation are separate modules. |
| Single Responsibility and Interface Segregation (GL 2, GL 3) | `sirius_physical_operator` does not gain a collection of distribution virtual methods. A nonintrusive planner pass owns the operation. |
| Design for testability (GL 4) | Compatibility, property derivation, metadata propagation, and exchange policy are value-based functions testable without a GPU. |
| Open/Closed, applied only at real variation points (GL 5, GL 15) | Source-layout discovery has a narrow adapter interface. Operator property rules remain centralized and fail closed instead of creating a hierarchy prematurely. |
| Liskov Substitution (GL 6) | A source adapter may claim a distribution only when it satisfies the full semantic contract. An approximate claim is not a valid substitute. |
| Dependency Inversion (GL 9) | High-level distribution contracts own the abstractions. Scan-manager and cuDF details depend on those contracts, not the reverse. |
| Prefer composition over inheritance (GL 20) | Exchange plans, runtime state, batch metadata, and layout leases are composed into existing objects. The current `operator_data` hierarchy is not expanded for every metadata combination. |
| Prefer value semantics (GL 22) | Specs, requirements, decisions, rejection reasons, IDs, and propagation rules are immutable values. Shared ownership is used only for query state and immutable cache generations. |
| Strategy, in value form (GL 19, GL 23) | Runtime reuse policy is an injected value object, not conditionals spread through PARTITION or a virtual hierarchy in the hot path. |
| Adapter (GL 24) | The pinned cache adapts its concrete entries to a high-level source-layout catalog. The GPU partition implementation adapts the semantic hash contract to cuDF. |
| Avoid Singleton and global state (GL 37, GL 38) | Runtime domains, exchange coordination, and leases are query-scoped. There is no process-wide distribution registry. |

Patterns are used for their intent, not as goals (GL 11, GL 12, GL 14). The analyzer
uses the operations-over-a-stable-type-set tradeoff discussed by GL 15 and GL 16, but
implements it as centralized enum dispatch; it is not the External Polymorphism
pattern from GL 31. An Observer hierarchy is unnecessary because the current
requirement is one existing logging/telemetry stream rather than independently
varying subscribers. The design also rejects a dynamic Strategy interface where a
value object or free function is sufficient (GL 23).

## Change Vectors

The design isolates the parts expected to change independently:

1. Operators that preserve or destroy a distribution.
2. Hash algorithms, key transforms, and semantic format versions.
3. Runtime policy thresholds and hardware topology.
4. Source layout types: Sirius pinned layouts first, external formats later.
5. GPU partition implementation details.
6. Repository scheduling and physical GPU placement.
7. Query-visible diagnostics and telemetry.

A change in any one of these should not require editing every physical operator or
teaching low-level cuDF code about planner concepts.

## Correctness Invariants

These are implementation invariants, not optimizer hints.

1. A partition token is created only by a validated source layout, a SHUFFLE exchange,
   or an operator propagation rule proven by the plan.
2. Compatibility compares all correctness-bearing fields: kind, ordered keys, key
   transforms, hash algorithm, seed, null policy, encoding/version, partition domain,
   and resolved count.
3. Every logical partition ID is less than its resolved partition count.
4. A runtime partition domain resolves once. A second resolution to a different count
   fails the query.
5. A coordinated join exchange chooses one decision for both sides before either side
   publishes a batch.
6. No token is manufactured from `preferred_device_id`, an affinity key, a
   repository index without a channel contract, or a GPU memory-space pointer.
7. Repository routing by logical partition validates the token against the channel's
   planned property.
8. The repository represents the complete logical domain, including leading, middle,
   and trailing empty partitions.
9. A pinned layout generation is immutable after publication. An executing query owns
   a lease that keeps exactly that generation alive.
10. UNKNOWN or ambiguous property derivation selects the existing SHUFFLE behavior.
11. A decision cannot change after publication starts. There is no mid-stream
    fallback.
12. Per-batch metadata is aligned one-to-one with the batches in
    `pipelineable_operator_data`.
13. BUILD_PROBE affinity is not semantic partitioning and never propagates through a
    join result.
14. Initial reuse is limited to the INNER equi-join and grouped-aggregate rules in the
    architecture document.

Violation of an internal invariant is a query error. A normal incompatibility is a
typed rejection and selects SHUFFLE.

## Architectural Boundaries

### Dependency direction

~~~text
                       +-------------------------------+
                       | planner::distribution_analyzer|
                       +---------------+---------------+
                                       |
                                       v
+----------------------+     +-------------------------+
| source_layout_catalog| --> | distribution value model|
+----------+-----------+     +------------+------------+
           ^                              |
           | implements                   v
+----------+-----------+     +-------------------------+
| scan_manager adapter |     | immutable distribution  |
| and pinned layouts   |     | plan + query state      |
+----------------------+     +------+-------------+----+
                                      |             |
                         configures    |             | queried by
                                      v             v
                              +-------+----+   +----+----------------+
                              | PARTITION  |   | metadata propagator |
                              | / CONCAT   |   | in pipeline task    |
                              +-------+----+   +---------------------+
                                      |
                                      v
                              +---------------+
                              | cuDF partition |
                              | implementation |
                              +---------------+
~~~

The core distribution model may depend on the C++ standard library and Sirius logical
types. It must not include cuDF, RMM, scan-manager, physical-operator, pipeline, or
engine headers.

The planner integration may depend on physical operators and the core model. The scan
manager and GPU mechanism are outer adapters and may depend inward on the model.

### Proposed modules

~~~text
src/include/distribution/
  distribution_spec.hpp
  distribution_compatibility.hpp
  distribution_plan.hpp
  exchange_policy.hpp
  runtime_distribution.hpp
  source_layout_catalog.hpp

src/distribution/
  distribution_compatibility.cpp
  exchange_policy.cpp
  runtime_distribution.cpp

src/include/planner/
  distribution_analyzer.hpp

src/planner/
  distribution_analyzer.cpp
~~~

Do not create a general framework under `distribution/`. These files cover
the concrete variation points required by issue #1018.

## Semantic Value Model

### Strong IDs and partition domains

Use distinct value types so a partition ID, domain ID, operator ID, and GPU ID cannot
be interchanged accidentally.

~~~cpp
struct partition_domain_id {
  std::uint64_t value;
  friend auto operator<=>(const partition_domain_id&, const partition_domain_id&) = default;
};

struct logical_partition_id {
  std::uint32_t value;
  friend auto operator<=>(const logical_partition_id&, const logical_partition_id&) = default;
};

struct fixed_partition_count {
  std::uint32_t value;
};

struct runtime_partition_domain {
  partition_domain_id id;
};

using partition_count_spec =
  std::variant<fixed_partition_count, runtime_partition_domain>;
~~~

Counts are positive. A fixed count of one is normalized to SINGLE where applicable.
A runtime domain ID is allocated by the planner and instantiated by the query.

### Distribution specification

The stable contract must not expose `cudf::data_type` or a cuDF enum.

~~~cpp
enum class distribution_kind : std::uint8_t {
  single,
  hash,
  key_grouped
};

enum class hash_algorithm_id : std::uint16_t {
  sirius_murmur3_32_v1
};

enum class null_hash_policy : std::uint8_t {
  sirius_null_hash_v1
};

enum class partition_transform_kind : std::uint8_t {
  identity,
  bucket,
  day
};

struct partition_transform {
  partition_transform_kind kind;
  sirius::logical_type result_type;
  std::optional<std::uint32_t> argument;
};

struct distribution_key {
  std::size_t output_column;
  partition_transform transform;
};

class distribution_spec {
 public:
  static distribution_spec single();

  static distribution_spec hash(
    std::vector<distribution_key> keys,
    hash_algorithm_id algorithm,
    std::uint32_t seed,
    null_hash_policy nulls,
    partition_count_spec partitions,
    std::uint32_t format_version);

  // Read-only accessors and value equality.

 private:
  // Construction is private so invalid combinations cannot be represented.
};

using provided_distribution =
  std::variant<unknown_distribution, distribution_spec>;
~~~

Exact descriptor equality is implemented once as a nonmember `operator==`
over the value members (GL 2's DRY rule; GL 4 makes it directly table-testable).
Use it only when exact identity is the intended relation.

Every reuse decision in the planner, layout catalog, and exchange goes through
`distribution_satisfies`. Compatibility is deliberately richer than
equality: it applies key correspondence, aggregate-key containment, fixed/runtime
domain rules, and resolved counts. It may use `operator==` as a fast path
and shared field-comparison helpers internally, but callers must not substitute exact
equality for requirement satisfaction or hand-roll either relation.

`sirius_murmur3_32_v1` names Sirius semantics, even if cuDF implements
them. Its specification includes value encoding, ordered key combination, bucket
mapping, and null behavior. If a future cuDF release changes any of those semantics,
Sirius introduces a new algorithm or format version instead of silently reusing the
old identifier.

For an intra-query property, `output_column` is always relative to the
producing operator's output. A persisted pinned-layout spec instead stores the key
relative to that layout's column list; scan binding remaps it to query-time output
ordinals and fails closed when a key is not projected.

The MVP accepts only the IDENTITY transform. Its `result_type` expresses the
cast, if any, applied before hashing. Later BUCKET or DAY transforms must define their
parameters and output type completely. The per-spec algorithm defines row-level
combination and value encoding over the transformed key vector.

`format_version` versions bucket-assignment semantics, not the C++ struct's
serialization schema. It changes whenever identical input could map to a different
logical partition, including a hash implementation or value-encoding change.

UNKNOWN is represented as a separate variant alternative, not a partially initialized
`distribution_spec`.

### Compatibility results

Expected incompatibility is data, not an exception and not a formatted string.

~~~cpp
enum class distribution_mismatch : std::uint8_t {
  none,
  unknown,
  kind,
  key_count,
  key_order,
  key_transform,
  key_type,
  algorithm,
  seed,
  null_policy,
  format_version,
  domain,
  partition_count,
  unsupported_mapping
};

struct compatibility_result {
  bool compatible;
  distribution_mismatch mismatch;
  std::optional<std::size_t> key_index;
};

compatibility_result distribution_satisfies(
  const distribution_spec& provided,
  const distribution_requirement& required,
  const key_correspondence& correspondence);
~~~

Formatting a diagnostic is a separate operation. Unit tests assert mismatch enums,
not log text.

## Plan Model

Plan construction has two stages. The analyzer produces an immutable
`distribution_analysis` for the original physical operators before pipeline
splitting. The converter uses that analysis plus its inserted operators to build
and then freeze the query distribution plan.

~~~cpp
struct distribution_property_id {
  std::uint32_t value;
};

struct distribution_analysis {
  // Original-operator properties, requirements, and source-layout leases.
};

enum class partition_effect : std::uint8_t {
  clear,
  preserve_task_partition,
  establish_and_validate
};

struct propagation_rule {
  partition_effect effect;
  std::optional<distribution_property_id> required_input;
  std::optional<distribution_property_id> provided_output;
};

enum class exchange_capability : std::uint8_t {
  shuffle_only,
  reuse_required,
  reuse_or_shuffle
};

enum class exchange_action : std::uint8_t {
  shuffle,
  reuse
};

enum class exchange_role : std::uint8_t {
  join_build,
  join_probe,
  grouped_aggregate,
  standalone
};

struct exchange_plan {
  exchange_id id;
  exchange_role role;
  exchange_capability capability;
  distribution_requirement requirement;
  distribution_property_id output_property;
  shuffle_transform shuffle;
  std::optional<exchange_group_id> group;
};

class distribution_plan {
 public:
  const propagation_rule& rule_for(std::size_t operator_id) const;
  const exchange_plan& exchange_for(std::size_t operator_id) const;
  const distribution_spec& property(distribution_property_id id) const;
};
~~~

The analyzer creates remapped properties for original operators. The converter adds
properties and rules for PARTITION, CONCAT, and repository channels through a private
builder, then exposes only the immutable `distribution_plan`. Runtime propagation
therefore does not repeat expression analysis; it validates a task partition and
replaces the input property handle with the already-derived output property.

### Nonintrusive property analysis

`planner::distribution_analyzer` is a read-only, bottom-up operation over
the physical plan. It uses the existing operator type and typed casts. It does not
mutate operators and it does not add distribution methods to the physical-operator
base class.

This is a deliberate operation-oriented design:

- Distribution is one operation over an existing family of physical types.
- Most operators do not need a distribution-specific public interface.
- A new unrecognized operator receives the safe `clear/UNKNOWN` rule.
- All propagation rules are visible and testable in one module.

This is preferable to forcing every operator to implement a wide interface. If
several independent property passes later require double dispatch, a Visitor can be
introduced then; issue #1018 alone does not justify it.

The initial derivation rules are those in the architecture document. In particular:

- FILTER preserves a task partition.
- Direct-reference PROJECTION remaps keys; a dropped or computed key clears.
- Eligible INNER HASH_JOIN maps an input join-key property to surviving output keys.
- Local HASH_GROUP_BY preserves when all distribution keys are grouping keys and
  remaps them to aggregate output ordinals.
- CONCAT preserves only when it does not fold partitions together. `concat_all`
  clears the input token; a separately proven one-partition result may normalize to
  SINGLE, while BUILD_PROBE remains affinity-only.
- MERGE_GROUP_BY may retain the exchange input property but never invents a wider key
  set.
- Unrecognized operators, other joins, and ambiguous expressions clear.

Grouped-aggregate reuse is limited to one grouping set and no grouping functions.
`GROUPING SETS`, `ROLLUP`, and `CUBE` force SHUFFLE,
because an individual set can omit a distribution key even when the aggregate's union
of group expressions contains it. Reuse uses the existing
`can_use_partitioned_aggregate` gate as a planner precondition.

Delim-internal exchanges also remain `shuffle_only`. Their internal
comparison join is not a normal plan-tree child, so the first analyzer pass cannot
prove its property by ordinary bottom-up traversal.

### Analyzer placement

Run analysis in `sirius_engine::initialize_internal` before
`sirius_meta_pipeline::build` and before the converter inserts PARTITION
and CONCAT. Pass the immutable `distribution_analysis` into
`sirius_pipeline_converter`.

The converter:

1. Reads consumer requirements from the analysis.
2. Constructs explicit `exchange_plan` values.
3. Assigns exchange and channel contracts to inserted operators.
4. Adds propagation rules for inserted PARTITION and CONCAT operators.
5. Freezes and returns the finalized plan in its conversion result.

PARTITION must no longer inspect its parent operator to discover keys or casts.

## Query-Scoped Runtime Model

### Runtime domain state

~~~cpp
enum class domain_status : std::uint8_t {
  unresolved,
  resolved,
  failed
};

enum class resolve_result : std::uint8_t {
  resolved_now,
  already_resolved_same,
  conflict
};

class partition_domain_state {
 public:
  resolve_result resolve(std::uint32_t count);
  std::optional<std::uint32_t> resolved_count() const;
  std::uint32_t require_resolved_count() const;

 private:
  mutable std::mutex mutex_;
  domain_status status_{domain_status::unresolved};
  std::optional<std::uint32_t> count_;
};
~~~

Fixed domains are instantiated resolved. Runtime domains start unresolved. No GPU task
blocks on a condition variable; scheduling hints keep dependent work waiting until the
coordinator has made a decision.

### Distribution instances and tokens

A plan property describes semantics. A runtime instance combines it with the query's
domain state.

~~~cpp
struct distribution_instance {
  std::shared_ptr<const distribution_spec> spec;
  std::shared_ptr<const partition_domain_state> domain;
};

class partition_token {
 public:
  static partition_token checked(
    std::shared_ptr<const distribution_instance> distribution,
    logical_partition_id partition);

  const distribution_instance& distribution() const noexcept;
  logical_partition_id partition() const noexcept;

 private:
  // No unchecked public constructor.
};
~~~

Pointer identity is an ownership optimization only. Compatibility uses semantic
values and the resolved count.

### Query ownership

`planner::query` owns a `query_distribution_context` containing:

- The immutable finalized distribution plan.
- One state object per runtime domain.
- One coordinator per exchange group.
- Selected source-layout leases.
- Decision diagnostics for the query.

Pipelines and task-global state hold shared handles only to keep the context alive
until in-flight tasks drain. The context has no references back to the query,
pipelines, or operators, so there is no ownership cycle.

The converter returns the plan/context seed. `sirius_engine` transfers it
through `SiriusContext::create_query` into `planner::query`.
No state is registered globally.

## Batch Metadata and Affinity

### Composition in operator data

Add metadata to `pipelineable_operator_data` rather than introducing
`partitioned_join_data`, `partitioned_scan_data`, and similar
subclasses.

~~~cpp
struct batch_metadata {
  std::optional<partition_token> partition;
};

class pipelineable_operator_data : public operator_data {
 public:
  pipelineable_operator_data(
    std::vector<std::shared_ptr<cucascade::data_batch>> batches,
    std::vector<batch_metadata> metadata);

  std::span<const batch_metadata> metadata() const noexcept;
  void replace_metadata(std::vector<batch_metadata> metadata);
};
~~~

Constructor and replacement enforce:

~~~text
metadata.size() == data_batches.size()
~~~

The existing one-argument constructors create one empty metadata value per batch.
That preserves current callers during migration.

`partitioned_operator_data` becomes a transitional adapter. Existing
consumers can be migrated incrementally, after which the subclass and its overloaded
integer can be removed.

### Interaction with PR #1038 slot coalescing

PR #1038 can combine several fine logical partitions into one per-GPU slot batch. A
single `partition_token` cannot truthfully describe that mixed batch, and
the repository slot is not a logical partition ID.

The initial implementation makes an explicit data-model choice: disable slot
coalescing on every exchange whose output property is propagated or considered for
reuse. SHUFFLE-only exchanges that do not expose a semantic output property may keep
the optimization. Add an `exchange_plan` flag expressing this requirement
so it is decided by planning, not by an operator-name check.

A future design may represent a partition-ID set and derive a valid coarsened
distribution, but that is outside issue #1018's first release. Reuse tests must run
with the #1038 path enabled and prove that a participating exchange selects the
non-coalescing representation.

### Scheduling affinity is separate

Add a strong generic affinity group for BUILD_PROBE:

~~~cpp
struct execution_affinity_group {
  std::uint64_t value;
};
~~~

These are scheduling conventions, not part of token validity. A token remains valid
after GPU-to-host-to-disk downgrade, migration, or restoration on another GPU. Task
placement uses this precedence:

1. A semantic partition token maps `p` to
   `active_gpu_ids[p % G]`.
2. An explicit execution-affinity group maps its stable key to one GPU.
3. `preferred_device_id` is a soft locality preference.
4. Existing scheduler fallback applies.

BUILD_PROBE sets an affinity group and carries no partition token. A pinned or fresh
partitioned scan carries a token; its preferred device should agree with the token's
mapping.

## Central Metadata Propagation

`gpu_pipeline_task::run_one_operator` applies the planned propagation rule
immediately after `execute` returns and before output logging, validation,
or sink publication.

~~~text
operator output
  -> apply_distribution_rule(operator_id, input, output, query_context)
  -> validate metadata alignment and token contract
  -> log
  -> publish to sink
~~~

Rule behavior:

- `clear`: attach empty metadata to every output batch.
- `preserve_task_partition`: require all semantic input tokens relevant
  to the task to have the same domain and logical ID, then attach the planned output
  property with that ID to every output batch.
- `establish_and_validate`: scan or PARTITION has already attached output
  tokens; validate them against the planned output property.

Cardinality changes are allowed when the whole task remains in one logical partition:

- FILTER and PROJECTION are normally one-to-one.
- HASH_JOIN may consume two batches and produce one batch.
- Local HASH_GROUP_BY may reduce one or more batches to one.
- CONCAT without `concat_all` may combine several batches only after
  validating that all belong to the same logical partition.
- CONCAT with `concat_all` clears input partition tokens. BUILD_PROBE output
  carries only its explicit affinity group.

No rule copies a token by vector position without validating the common task
partition.

## Distribution-Aware Repository Channels

### Why a channel contract is required

Today repositories store `data_batch` only. The default sink drops
`operator_data` metadata and inserts every batch into repository partition
zero. Central in-pipeline propagation alone is therefore insufficient: the
join-to-aggregate token would disappear at the FULL barrier before PARTITION.

Do not put Sirius planner metadata into the generic cuCascade `data_batch`.
Instead, make distribution preservation an explicit property of a repository edge.

~~~cpp
enum class repository_routing : std::uint8_t {
  unpartitioned,
  logical_partition
};

struct distribution_channel {
  repository_routing routing;
  distribution_property_id property;
};

struct repository_wiring {
  // Existing topology fields...
  std::optional<distribution_channel> distribution;
};
~~~

The materializer installs the same channel contract on the source's
`next_port_info` and the destination `port`.

### Publication

For an unpartitioned channel, behavior remains unchanged.

For a logical-partition channel, the common sink:

1. Reads the metadata aligned with each batch.
2. Requires a token.
3. Validates that token against the channel property.
4. Requires the domain to be resolved.
5. Ensures the repository can represent the complete count.
6. Calls `push_data_batch_partitioned(..., token.partition())`.

### Consumption

A partition-aware source enumerates repository partitions. For partition `p`
it reconstructs a checked token from the port's channel property and `p`.
The repository index is evidence only because the edge has an explicit, validated
channel contract.

This avoids storing duplicated metadata on every repository entry while preserving
the proof across barriers.

### Empty partition support

Add the following operation to cuCascade's repository owner:

~~~cpp
void ensure_num_partitions(std::size_t count);
~~~

It is thread-safe, rejects zero, grows but never shrinks, and does not create dummy
batches. Sirius validates that an already-larger repository is not being bound to a
smaller domain.

This belongs on `idata_repository` because the repository owns the vector
and expected cardinality is a fundamental storage operation. A Sirius subclass or
`dynamic_cast` adapter would weaken the contract.

Consumers must skip empty partitions rather than return `nullptr` at the
first empty partition. In particular,
`sirius_physical_grouped_aggregate_merge::get_next_task_input_data` must
continue scanning later partition IDs.

## Exchange Design

### Responsibility split

The current `sirius_physical_partition` derives keys by inspecting a parent,
chooses counts, locks a sibling, mutates hash-join mode, toggles CONCAT behavior,
executes a GPU kernel, and routes output. Replace that cluster with composed parts:

| Component | Responsibility |
|---|---|
| `exchange_plan` | Immutable requirement, capability, key transform, property IDs, and group ID. |
| `exchange_policy` | Pure value Strategy choosing reuse or shuffle from compatible properties and actual stats. |
| `partition_domain_state` | Resolve and expose one query-scoped logical count. |
| `join_exchange_coordinator` | Atomically decide both join sides and expose join/CONCAT mode. |
| `sirius_physical_partition` | Execute the chosen forwarding or GPU partition mechanism and publish tokens. |
| `gpu_partition_impl` | Translate Sirius key transforms to cuDF and run the kernel. |

The PARTITION constructor becomes conceptually:

~~~cpp
sirius_physical_partition(
  duckdb::vector<sirius::logical_type> types,
  std::size_t estimated_cardinality,
  exchange_plan plan,
  std::shared_ptr<exchange_runtime_state> runtime);
~~~

It no longer stores parent, sibling, hash-join pointers, or
`std::vector<cudf::data_type>` in its public header.

### Value-based Strategy

~~~cpp
struct partition_stats {
  std::uint64_t total_bytes;
  std::uint64_t max_partition_bytes;
  std::vector<std::uint64_t> bytes_by_partition;
  std::uint32_t nonempty_partitions;
};

struct exchange_policy_input {
  compatibility_result compatibility;
  partition_stats actual;
  std::uint32_t active_gpus;
  std::uint64_t target_partition_bytes;
  exchange_capability capability;
};

enum class exchange_reason : std::uint8_t {
  required_shuffle,
  compatible_reuse,
  incompatible_distribution,
  insufficient_parallelism,
  oversized_partition,
  skew,
  source_layout_unavailable
};

struct exchange_decision {
  exchange_action action;
  exchange_reason reason;
};

class exchange_policy {
 public:
  exchange_decision choose(const exchange_policy_input&) const;

 private:
  exchange_policy_config config_;
};
~~~

This is Strategy by intent, implemented as a small value. It is injected into planning
and runtime setup. There is no virtual call or `std::function` in the
per-batch GPU path.

Correctness compatibility is evaluated before policy. Policy cannot turn an
incompatible layout into reuse.

### Planned capability and resolved action

Do not use one enum for both plan intent and runtime state.

- `shuffle_only` always resolves to SHUFFLE.
- `reuse_required` resolves to REUSE; a missing token is an internal error.
- `reuse_or_shuffle` evaluates compatibility, source availability, and
  actual partition stats exactly once before publication.

The user-visible mode is the resolved `exchange_action`.

The Q18-like aggregate exchange uses `reuse_or_shuffle` because the existing
FULL barrier provides actual post-join partition sizes. Scan-side selected layouts
also always use `reuse_or_shuffle`, giving the pair one pre-publication
decision for lease validation, residency, and policy. Eager acquisition keeps the
chosen generation alive but does not introduce a second scan exchange mode.

### Exchange runtime state

~~~text
PLANNED
   |
   +---- decision succeeds ----> DECIDED_REUSE ----+
   |                                               |
   +---- decision succeeds ----> DECIDED_SHUFFLE --+--> PUBLISHING --> COMPLETE
   |
   +---- invariant/config error ------------------------> FAILED
~~~

Transitions are monotonic under one mutex. `begin_publication` is legal only
from a decided state. Decision mutation after publication is an invariant failure.

### Execute behavior

REUSE:

1. Require one input logical partition per task.
2. Validate its token and channel property against the exchange requirement.
3. Forward the existing batch/read-only handle without hashing or cloning when
   possible.
4. Attach the exchange output property with the same logical ID.

SHUFFLE:

1. Read the resolved output count.
2. Translate Sirius key transforms to cuDF types in the `.cpp` adapter.
3. Run `gpu_partition_impl::hash_partition`.
4. Attach explicit IDs `0..N-1` to returned batches.
5. Publish by token ID, never by local loop position alone.

For `N == 1`, establish a checked SINGLE token even when the data batch is
forwarded without a kernel.

### Memory estimation

`no_history_peak_memory_estimate` reads the resolved action:

- REUSE: zero additional partition materialization, excluding ordinary input
  materialization already accounted for by the pipeline.
- SHUFFLE with `N > 1`: retain the current conservative estimate.
- Undecided: use the SHUFFLE estimate.

This prevents a speculative reuse decision from under-reserving memory.

## Join Exchange Coordination

Replace raw sibling pointers and ABBA-sensitive locking with one shared coordinator.

~~~cpp
struct join_exchange_decision {
  exchange_action build_action;
  exchange_action probe_action;
  std::uint32_t partition_count;
  join_execution_mode join_mode;
  bool concat_build_all;
};

class join_exchange_coordinator {
 public:
  decision_status try_decide(const join_exchange_observation&);
  std::optional<join_exchange_decision> decision() const;
  join_exchange_decision require_decision() const;
};
~~~

The build and probe PARTITION operators, both CONCAT operators, and HASH_JOIN compose
the same coordinator.

- The build exchange supplies actual build bytes when runtime sizing is needed.
- Source-layout leases and fixed specs are already in the query context.
- The coordinator resolves the shared domain and both exchange actions under one
  lock.
- Probe scheduling waits for a decision instead of calling through a sibling pointer.
- CONCAT reads `concat_build_all`; PARTITION does not mutate CONCAT.
- HASH_JOIN reads `join_mode`; PARTITION does not mutate HASH_JOIN.

For one-sided Sirius layout reuse, the pinned side's fixed spec is authoritative and
the opposite action is SHUFFLE using that exact transform. For two incompatible or
unreproducible source layouts, both actions are SHUFFLE with a new Sirius domain.

Unsupported join forms retain current behavior and do not receive a reusable output
property.

## Intra-Query Join-to-Aggregate Sequence

The full path is:

~~~text
1. Join exchanges resolve domain D to N.
2. HASH_JOIN task p is created from compatible build/probe channel partitions.
3. The join task carries token (D, p); BUILD_PROBE carries affinity only.
4. HASH_JOIN executes.
5. The central rule attaches the remapped join-output property with p.
6. FILTER / direct PROJECTION preserve and remap p.
7. Local HASH_GROUP_BY preserves p when its grouping keys satisfy the rule.
8. The group-by sink publishes through a logical-partition repository channel.
9. The aggregate PARTITION waits at the existing FULL barrier.
10. It gathers actual bytes by p and chooses REUSE or SHUFFLE once.
11. REUSE forwards p; SHUFFLE establishes a new domain.
12. MERGE_GROUP_BY consumes each nonempty logical partition.
~~~

The barrier channel in steps 8-10 is essential. Without it, all local aggregate
outputs collapse into repository partition zero and the proof is lost.

## Source Layout Adapter and Pinned Generations

### High-level catalog

The planner needs layout descriptors, not scan-manager internals.

~~~cpp
struct source_layout_request {
  source_identity source;
  std::vector<source_column_id> required_columns;
};

struct source_layout_descriptor {
  source_layout_id id;
  std::uint64_t generation;
  distribution_spec distribution;
  partition_stats stats;
};

class source_layout_lease {
 public:
  virtual ~source_layout_lease() = default;
  virtual const source_layout_descriptor& descriptor() const noexcept = 0;
};

struct source_layout_catalog_result {
  std::vector<std::shared_ptr<const source_layout_lease>> candidates;
  std::vector<source_layout_rejection> rejections;
};

class source_layout_catalog {
 public:
  virtual ~source_layout_catalog() = default;

  source_layout_catalog_result acquire_candidates(
    const source_layout_request&) const;

 private:
  virtual std::vector<std::shared_ptr<const source_layout_lease>>
  do_acquire_candidates(const source_layout_request&) const = 0;
};
~~~

The abstraction is owned by the high-level distribution module. Scan manager
implements it as an object Adapter. This virtual boundary is acceptable because it is
used during planning/preparation, not per batch.

Use the Non-Virtual Interface form shown by the GL 13
`std::pmr::memory_resource` example. The public, non-virtual
`acquire_candidates` calls the private virtual
`do_acquire_candidates`, applies generic descriptor checks such as spec
well-formedness, partition-stat shape, and duplicate layout/generation identities,
and returns typed rejection
values with the accepted leases. The caller turns those values into diagnostics; the
catalog has no logging or telemetry dependency.

Generic validation does not prove that storage actually implements its declared
distribution. Each concrete adapter remains responsible for proving its source
mapping before returning a lease, and adapter contract tests verify that proof. The
NVI guarantees common structural checks and reporting, not semantic truth that only
the adapter can establish.

Returning leased candidates makes discovery and lifetime acquisition one operation.
The analyzer deterministically sorts the compatible entries in
`source_layout_catalog_result::candidates` and retains only the selected
lease. Unselected leases are released immediately.

### Immutable pinned entry

Extend pinned entries with:

~~~cpp
struct pinned_entry {
  source_layout_descriptor layout;
  cache_entry_info cache_info;
  std::vector<logical_partition_id> chunk_partition_ids;

  // Existing GPU or host payload, immutable after publication.
};
~~~

`chunk_partition_ids[i]` is aligned with chunk index `i` across
all columns, memory spaces, or host chunks. Several chunks may share one logical ID;
empty logical partitions have no chunk.

Generation is monotonically increasing for a pinned name. Re-pinning or merging
columns publishes a new generation and never mutates one already visible to a query.

Change the map to:

~~~cpp
std::unordered_map<std::string, std::shared_ptr<const pinned_entry>> pinned_entries_;
mutable std::shared_mutex pinned_entries_mutex_;
~~~

Insertion or column merge builds a new generation and atomically replaces the map
pointer. Existing GPU columns are shared into the new generation; data is not copied
merely to publish metadata. Removal erases the map reference. Active query leases keep
the old generation alive until their providers and query finish.

A lease protects content identity and lifetime, not physical residency. The
partition token remains semantically valid if a chunk moves between GPU, host, and
disk tiers. Initially, bucketed pinned generations retain the existing configured-tier
pinning behavior; memory accounting charges every simultaneously live generation
until its last map reference and query lease are released. If bucketed entries become
downgrade candidates later, downgrade/upgrade must preserve the generation and
logical ID and should restore toward the GPU selected by `p % G` as a
locality preference.

The existing cached provider must own
`std::shared_ptr<const pinned_entry>`, not `const pinned_entry&`.
That removes the current dangling-reference risk when an entry is replaced or removed.

Visitor APIs snapshot shared pointers under the lock and invoke callbacks after
releasing it.

### Deterministic selection

The current unordered-map first match is not a planning policy. Selection is:

1. Match exact source identity.
2. Require requested-column coverage.
3. Require semantic compatibility with the downstream requirement.
4. Apply the value-based cost policy.
5. Tie-break by a documented stable order, such as layout name, then generation.
6. Bind the exact lease in the query context.

If no lease is acquired, planning selects SHUFFLE. Replacement after planning does not
force fallback because the lease keeps the chosen snapshot alive.

A selected scan exchange is still planned as `reuse_or_shuffle`. During
`SiriusContext::create_query`, scan preparation validates the lease and
resolves the coordinated action before `start_query` can schedule a source
task. There is no mid-query replan.

### Partition-aware cached provider

Change the cold provider value from a bare batch to:

~~~cpp
struct provided_batch {
  std::shared_ptr<cucascade::data_batch> batch;
  std::optional<source_partition_claim> partition;
};
~~~

`source_partition_claim` contains the selected layout property and logical
ID. It is added to the format-independent `scan_operator_input`, not to
every format-specific `scan_info` subclass.

The cached provider emits one claim per chunk. The load-balancing coalescer derives
the target GPU from the logical ID. The common GPU scan operator attaches a checked
partition token to its output after materialization and projection.

This placement keeps Parquet and DuckDB-native scan interfaces narrow.

### Pin-time bucket creation

For the initial Sirius-created layout:

1. Validate `bucket_by` keys and a positive fixed bucket count.
2. Use the same semantic hash version and low-level GPU adapter as SHUFFLE.
3. Record explicit logical IDs for nonempty output chunks.
4. Place partition `p` on the GPU selected by `p % G`.
5. Publish a new immutable generation only after every column/chunk vector validates.

Partial publication is not visible to planning.

## Error and Fallback Policy

| Condition | Handling |
|---|---|
| Plan property is UNKNOWN | Plan SHUFFLE. |
| Two specs differ | Return typed mismatch; plan or decide SHUFFLE. |
| No pinned candidate can be leased | Plan SHUFFLE before tasks start. |
| Internal REUSE input has no token | Fail query as an invariant violation. |
| Token ID is outside resolved domain | Fail query. |
| Runtime domain resolves to conflicting counts | Mark state failed and fail query. |
| Channel receives a token for another property | Fail query before repository insertion. |
| Policy rejects compatible layout before publication | Resolve action to SHUFFLE. |
| Any member of a join exchange has published | Coordinated decision is immutable; later fallback is forbidden. |
| GPU SHUFFLE OOM | Existing reservation and retry behavior applies. |
| Unsupported join/operator/source transform | Clear property and retain current path. |

Exceptions are reserved for invariant failures and execution errors. Normal
compatibility and policy rejection use value results.

## Observability

Emit immutable event values through existing logging and telemetry. Do not add an
Observer hierarchy for one consumer.

~~~cpp
struct exchange_decision_event {
  std::size_t operator_id;
  exchange_action action;
  exchange_reason reason;
  distribution_property_id input_property;
  distribution_property_id output_property;
  std::uint32_t partition_count;
  partition_stats stats;
};
~~~

The plan printer should show:

~~~text
PARTITION[capability=REUSE_OR_SHUFFLE, domain=d3, keys=(#0)]
PARTITION[action=REUSE, partitions=8, reason=compatible_reuse]
~~~

Runtime telemetry should include kernel invocation count, bytes hashed/materialized,
logical partition count, nonempty count, maximum partition bytes, and bytes moved
between GPUs.

## Concurrency and Lifetime

### Locking rules

- `partition_domain_state` protects one monotonic resolution.
- `exchange_runtime_state` protects one monotonic action transition.
- `join_exchange_coordinator` protects the pair decision with one mutex.
- No exchange takes another exchange's mutex.
- Pinned map mutation uses its own shared mutex and never calls external code while
  holding it.
- Repository locks remain internal to cuCascade.

This removes the current sibling ABBA concern from PARTITION.

### Publication rule

The code that changes a decision to PUBLISHING and the code that exposes the first
batch must be ordered under the exchange state. A practical implementation is:

1. `begin_publication(action)` validates the decided action and sets a
   monotonic published flag.
2. Sink publication follows.
3. Any later attempt to alter the action sees the flag and fails.

No lock is held while a GPU kernel runs or while a batch is inserted into a repository.

### Ownership summary

| Object | Owner |
|---|---|
| Distribution specs and plan rules | Immutable finalized plan shared by query/tasks |
| Runtime domain and exchange states | Query distribution context |
| Join coordinator | Query distribution context; composed by join exchange operators |
| Source layout generation | Pinned map plus active leases |
| Selected layout lease | Query context and cached provider |
| Operator batches | Existing cuCascade shared ownership |
| Repositories | Existing data repository manager |
| GPU kernel temporaries | Existing task/stream scope |

## Hot-Path Cost Model

GL 29, GL 33, GL 34, and GL 36 motivate making runtime costs explicit and measuring
them; they do not justify zero-cost claims before implementation. The expected
feature-specific work per task or batch is:

| Operation | Expected cost and constraint |
|---|---|
| Token copy on batch hand-off | One nontrivial `shared_ptr` copy/destruction, including reference-count traffic, plus one 32-bit ID |
| REUSE input validation | Interned-instance pointer fast path when available, semantic compatibility across distinct instances, resolved-count read, and ID range check |
| Central propagation | Rule lookup by operator ID plus aligned metadata construction or update; no expression analysis |
| Channel publication | Token validation followed by existing repository insertion and repository locking |

The source-layout virtual call and exchange policy run during planning or one-time
decision setup, not for each batch. The metadata path should not add
`std::function` or configuration reads. Whether it uses a type-tag cast,
performs synchronization, or allocates depends on the final implementation and must
be measured rather than asserted here.

The proposed `std::vector<batch_metadata>` API permits an allocation when
capacity is insufficient. Stage 1 records metadata allocation count, token
copy/destruction cost, and propagation time in a focused benchmark. Optimize only if
the measurements justify it, in this order:

1. Construct output metadata with the output batch vector and move both together.
2. Reserve or reuse metadata capacity across operator output construction.
3. Consider inline metadata storage for the common one-batch case.
4. Consider plan-arena handles only after proving their lifetime through task drain
   and repository boundaries.

The acceptance target is no material regression on forced-SHUFFLE workloads. The
design does not require a raw-pointer representation to meet that target.

## Test Architecture

### CPU-only unit tests

Add `test/cpp/distribution/` tests for:

- Valid and invalid spec construction.
- Exact descriptor equality, requirement satisfaction, and every
  `distribution_mismatch` reason.
- Fixed-count and shared-runtime-domain compatibility.
- Conflicting domain resolution under concurrency.
- Projection key reorder/drop/compute rules.
- Join output key correspondence and ambiguity.
- Grouped-aggregate superset rule.
- Grouping sets, rollup, cube, and grouping functions forcing SHUFFLE.
- SINGLE normalization.
- Value-policy decisions for size, skew, and GPU count.
- Propagation from N inputs to M outputs in one task partition.
- Rejection of mixed-token task inputs.
- BUILD_PROBE affinity never producing a token.

Use table-driven Catch2 cases. These tests include no CUDA headers and require no GPU.

### Repository and pipeline tests

Extend:

- `test/cpp/pipeline/test_repository_wiring_materializer.cpp` for channel
  materialization.
- Add a barrier round-trip test proving token `p` is routed into repository
  partition `p` and reconstructed.
- Cover all-empty, leading-empty, middle-empty, and trailing-empty domains.
- Verify an unpartitioned channel retains current partition-zero behavior.
- Verify a mismatched token is rejected before insertion.
- Verify metadata is applied before sink publication.
- Verify a reuse-participating exchange disables #1038 slot coalescing while an
  unrelated SHUFFLE-only exchange may retain it.

Add cuCascade tests for concurrent `ensure_num_partitions` and non-shrinking
behavior.

### Operator tests

Extend:

- `test/cpp/operator/test_physical_partition.cpp`:
  REUSE does not invoke the hash kernel, SHUFFLE remains unchanged, `N=1`
  establishes a token, and sink routes by ID rather than vector order.
- `test/cpp/operator/test_physical_concat.cpp`:
  ordinary inputs must share one token and output preserves it; `concat_all`
  clears semantic tokens and preserves only affinity where required.
- `test/cpp/operator/test_physical_hash_join_mgpu.cpp`:
  eligible INNER output preserves p; BUILD_PROBE and unsupported joins do not.
- `test/cpp/operator/test_physical_grouped_aggregate_merge_mgpu.cpp`:
  empty partitions are skipped and later partitions still run.
- `test/cpp/creator/test_task_creator.cpp`:
  token, affinity-group, and soft-preference precedence.

### Scan and lifetime tests

Add tests that:

- Candidate selection is deterministic regardless of unordered-map insertion order.
- A provider keeps generation 1 alive after generation 2 replaces it.
- Removal prevents new leases but does not invalidate an active lease.
- Column merge publishes a new immutable generation.
- Cached chunks emit their explicit logical IDs.
- Scan output contains the selected property and ID.
- Device selection is `p % G`.

Run lifetime/concurrency cases under ASan and TSan where supported.

### Integration tests

Add a Q18-like integration case with a test hook/counter proving:

1. Results equal DuckDB CPU execution.
2. The join exchange hashes as expected.
3. The aggregate exchange resolves to REUSE.
4. No second hash-partition kernel runs.

Also cover null and duplicate keys, direct projections, dropped keys, several batches
per partition, empty partitions, incompatible versions/counts/seeds, and a
policy-forced reshuffle.

Pinned-layout integration then covers two-sided reuse, one-sided fixed-spec reuse,
generation replacement, host tier, and at least two GPUs.

### Architecture fitness checks

Add a lightweight CI check that core headers under `src/include/distribution/`
do not include cuDF, RMM, scan-manager, operator, pipeline, or engine headers.

New operator enum values must receive either an explicit analyzer rule or the tested
UNKNOWN fallback. A default-to-preserve rule is forbidden.

## File-by-File Change Map

### New files

- `src/include/distribution/distribution_spec.hpp` and
  `src/distribution/distribution_compatibility.cpp`: semantic values and
  pure compatibility.
- `src/include/distribution/distribution_plan.hpp`: properties,
  propagation rules, exchanges, and channels.
- `src/include/distribution/runtime_distribution.hpp` and
  `src/distribution/runtime_distribution.cpp`: query domains, tokens,
  exchange state, and join coordinator.
- `src/include/distribution/exchange_policy.hpp` and
  `src/distribution/exchange_policy.cpp`: value Strategy.
- `src/include/distribution/source_layout_catalog.hpp`: high-level Adapter
  contract.
- `src/include/planner/distribution_analyzer.hpp` and
  `src/planner/distribution_analyzer.cpp`: nonintrusive property pass.
- `test/cpp/distribution/*.cpp`: CPU-only contract tests.

### Existing files with focused changes

- `src/include/op/sirius_physical_operator.hpp`:
  compose aligned batch metadata and generic affinity.
- `src/pipeline/gpu_pipeline_task.cpp`:
  apply and validate the central propagation rule.
- `src/include/pipeline/repository_wiring.hpp` and
  `src/pipeline/repository_wiring_materializer.cpp`:
  carry distribution-channel contracts.
- `cucascade/include/cucascade/data/data_repository.hpp`:
  add explicit partition-count growth.
- `src/include/op/sirius_physical_partition.hpp` and implementation:
  consume an exchange plan/state; remove parent introspection and sibling locking.
- `src/include/op/sirius_physical_concat.hpp` and implementation:
  consume a channel/coordinator; preserve one validated task partition.
- `src/op/sirius_physical_hash_join.cpp`:
  reconstruct join task proof, read coordinator mode, and expose eligible output
  propagation.
- `src/op/sirius_physical_grouped_aggregate_merge.cpp`:
  consume the full logical domain and skip empty partitions correctly.
- `src/include/pipeline/sirius_pipeline_converter.hpp` and implementation:
  consume the distribution plan, create exchange plans, and remove raw sibling links.
- `src/include/planner/query.hpp`,
  `src/sirius_context.cpp`, and `src/sirius_engine.cpp`:
  transfer query-scoped context ownership.
- `src/include/scan_manager/sirius_scan_manager.hpp` and implementation:
  implement the layout catalog and immutable generation leases.
- `src/include/scan_manager/load_balancing_scan_batch_coalescer.hpp` and
  implementation: emit partition-aware provided batches.
- `src/include/op/scan/sirius_gpu_scan_operator_data.hpp` and
  `src/op/scan/sirius_gpu_scan_operator.cpp`:
  carry and establish source claims.
- `src/pin_table.cpp`:
  create bucketed layouts and publish complete immutable generations.
- `CMakeLists.txt`:
  list new sources and tests.

No files under `src/legacy` are part of this work.

## Staged Implementation

### Stage 0: characterization

- Add tests for current SHUFFLE results, partition count selection, join mode, CONCAT,
  and empty partition behavior.
- Add a kernel-invocation counter usable only by tests/telemetry.
- Preserve benchmark artifacts and the exact baseline configuration.

### Stage 1: contracts with behavior unchanged

- Land the semantic model, compatibility results, runtime domains, affinity split,
  and unit tests.
- Add batch metadata, central clear/validate plumbing, channel descriptors, and
  `ensure_num_partitions`.
- Configure every exchange as `shuffle_only`.
- Migrate `partitioned_operator_data` call sites behind a compatibility
  adapter.

Acceptance: all existing queries and placement tests behave as before.

### Stage 2: property analysis in shadow mode

- Add the analyzer and plan-printer annotations.
- Derive would-reuse decisions but force SHUFFLE.
- Compare analyzer decisions with runtime tokens and emit mismatch diagnostics.

Acceptance: no semantic claim is observed without a matching runtime proof.

### Stage 3: intra-query aggregate reuse

- Enable join output, filter/projection, and local aggregate propagation.
- Enable the logical-partition channel at the aggregate FULL barrier.
- Resolve aggregate exchange policy from actual partition stats.
- Gate with one config flag and start with Q18-like shapes.

Acceptance: CPU-equivalent results and zero second partition-kernel invocations.

### Stage 4: immutable bucketed pinned layouts

- Add pin-time bucket creation, explicit chunk IDs, immutable generations, catalog
  leases, deterministic selection, and partition-aware scan inputs.
- Keep source-join reuse disabled until lifetime and scan tests pass.

### Stage 5: source join reuse

- Enable coordinated two-sided and one-sided reuse for exact Sirius specs.
- Keep non-INNER joins and external transforms on SHUFFLE.
- Add multi-GPU locality and transfer measurements.

Each stage is independently shippable and preserves a correctness-first fallback.

## Rejected Alternatives

### Add distribution virtual methods to every physical operator

Rejected because it widens a central interface for an orthogonal operation, couples
operators to planner property types, and makes unrelated operators change. The
nonintrusive pass is smaller and safer.

### Store only `partition_idx`

Rejected because the integer lacks keys, algorithm, version, count, and provenance,
and is already overloaded for BUILD_PROBE affinity.

### Put tokens directly on cuCascade `data_batch`

Rejected because it reverses the dependency direction and makes a generic memory/data
container understand Sirius planner semantics. Explicit repository channels preserve
the proof at the Sirius boundary.

### Infer partition IDs from output vector order

Rejected because reused and source data may omit empty partitions, batches can arrive
out of order, and one partition may contain several chunks.

### Use raw sibling pointers for coordinated decisions

Rejected because ownership is implicit, locking spans two objects, and PARTITION
mutates JOIN and CONCAT behavior. One shared coordinator owns the invariant.

### Use a process-wide domain or layout registry

Rejected because it creates hidden dependencies, complicates cleanup, and permits
cross-query state leaks. Query ownership and RAII leases are sufficient.

### Defer pinned generation acquisition until execution

Rejected for Sirius-owned pinned layouts. Eager immutable leases remove the
planning/execution race and make unpin semantics deterministic. Future external
catalogs that cannot lease must use `reuse_or_shuffle` and decide before
publication.

### Remove PARTITION and CONCAT in the first release

Rejected because those boundaries currently own repository routing, count visibility,
coalescing, mode selection, and scheduling behavior. REUSE first changes the
mechanism, not the DAG topology.

### Introduce polymorphic Strategy and Observer hierarchies

Rejected because exchange policy is one small value Strategy and telemetry has one
existing sink. Additional runtime polymorphism would add dependencies without a real
variation need (GL 12, GL 23, GL 25).

### Probe operators for a distribution capability via `dynamic_cast`

A `distribution_aware` mixin implemented by some operators and discovered with
`dynamic_cast` in the analyzer is a capability cross-cast, not the Acyclic
Visitor pattern from GL 18. It still reintroduces intrusive edits to operator classes
and replaces the analyzer's existing type-tag dispatch with another runtime
mechanism. Reject it because it adds no needed variation point; do not generalize
that decision into a claim that all runtime casts are forbidden.

### CRTP or compile-time dispatch for operators or rules

The analyzer walks heterogeneous operators behind the existing virtual base; the
resulting CRTP instantiations would not provide one common runtime base (GL 26).
Plan-time dispatch cost is irrelevant, so compile-time abstraction buys nothing here
(GL 36). Strong-ID types are the one CRTP-adjacent idea retained, in the value-type
form above.

### Pimpl, Bridge, or Type Erasure on `distribution_spec` / `partition_token`

A conventional owning Pimpl would add pointer indirection and would commonly add an
allocation, but neither Pimpl nor Bridge inherently requires one (GL 28, GL 29).
There is no required implementation variation here to justify either. Type Erasure
also complicates binary operations such as exact equality (GL 32). Keep both types as
plain values; the compile-time firewall belongs at the module boundary through the
fitness check on `src/include/distribution/` includes.

## Definition of Done

The initial implementation is complete when:

1. Core compatibility and propagation logic is covered by CPU-only tests.
2. Logical partition and BUILD_PROBE affinity are distinct types and paths.
3. Tokens survive an actual repository barrier through a validated channel.
4. Empty logical partitions are represented without dummy tables.
5. Join exchange decisions are atomic and use no raw sibling pointers.
6. An eligible INNER join-to-grouped-aggregate query returns CPU-equivalent results.
7. The aggregate exchange reports REUSE and invokes no hash-partition kernel.
8. Every unknown, ambiguous, unsupported, or policy-rejected case uses SHUFFLE.
9. Existing SHUFFLE and OOM retry behavior remains covered.
10. No global distribution state or pinned-entry reference lifetime hazard is
    introduced.

## Implementation Recommendation

Start with the semantic core and repository-channel round trip, then migrate existing
SHUFFLE execution onto the new plan/state contracts before enabling reuse. This puts
the dependency and lifetime boundaries under test while behavior is still unchanged.

Enable only the join-to-grouped-aggregate path next. Once that path proves token
propagation, barrier routing, runtime policy, and empty-domain handling, add immutable
Sirius bucketed pinned layouts and source join reuse. External layout adapters remain
a later extension point and must satisfy the same semantic contract.
