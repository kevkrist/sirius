# Shuffle Elision with Proven Distribution

**Status:** Revised design for review (r2, 2026-07-02: updated after a code-verified adversarial review)<br>
**Issue:** [#1018 - Explore avoiding shuffles with pre-partitioned data](https://github.com/sirius-db/sirius/issues/1018)<br>
**Related work:** [#995 - Avoid small copies during multi-GPU partitioning](https://github.com/sirius-db/sirius/issues/995), [#485 - Multi-partition outer joins](https://github.com/sirius-db/sirius/issues/485), [#1038 - Partition coalescing and single materialization](https://github.com/sirius-db/sirius/pull/1038), [#400 - Input data organization](https://github.com/sirius-db/sirius/issues/400)<br>
**Phase 0 evidence:** [issue-1018-shuffle-elision-design.md](issue-1018-shuffle-elision-design.md) (measurement note and reproduction recipe)<br>
**Implementation design:** [Issue #1018 Software Design: Distribution-Aware Exchange](issue-1018-software-design.md)

## Decision Summary

Sirius should represent physical distribution as a correctness-bearing property and
let the existing PARTITION boundary select one of three modes:

- **SHUFFLE:** Compute a new hash assignment and materialize partitioned batches.
- **REUSE:** Validate and forward a distribution established by Sirius earlier in the
  same query.
- **REUSE_OR_SHUFFLE:** Reuse a selected source layout when it is available and valid,
  otherwise choose the existing shuffle path before publishing any output.

Delivery should begin with intra-query reuse, not storage layout work. The first target
is an INNER hash join followed by a grouped aggregate whose grouping keys contain the
join distribution keys. Sirius already paid for the join shuffle; preserving that
distribution avoids hashing and materializing the aggregate input again.

The next target is a Sirius-created bucketed pinned layout. Sirius controls the hash
algorithm, key encoding, null behavior, partition count, and generation metadata, so
the layout can be trusted as a physical property. External Iceberg, Hive, or sidecar
metadata should follow only after the property and runtime contracts are proven.

PARTITION, CONCAT, and MERGE_GROUP_BY remain in the initial execution plans.
PARTITION(REUSE) is a validation, cost-policy, repository, and routing boundary rather
than a cuDF hash-partition kernel. Retaining that boundary avoids a broad pipeline-DAG
rewrite and preserves an atomic fallback point.

## Review Conclusions

The parallel shuffle-elision draft added three useful ideas that this design adopts:

- Propagate a Sirius-created distribution through a join and reuse it before building
  any storage-level feature.
- Treat one pre-partitioned join side as authoritative when Sirius can shuffle the
  other side with the exact same transform and fixed partition count.
- Use the reported Phase 0 measurements to prioritize multi-GPU and memory-pressured
  cases.

Five details require correction or tightening:

- Hash-join task inputs carry a partition index today, but hash-join results are plain
  pipelineable_operator_data. Filter, projection, and local grouped-aggregate results
  also drop the index. Output-token propagation is new implementation work.
- The existing partition index is overloaded. STANDARD/MIXED joins use a logical
  input partition, while BUILD_PROBE uses the join operator ID as an affinity key.
  Semantic partition identity must not be inferred from that field.
- Removing PARTITION and CONCAT entirely would bypass repository partition counts,
  empty-partition handling, coalescing, and join-mode assumptions. Initial reuse
  should keep these operators and make PARTITION a forwarding boundary.
- A minimal keys-and-count descriptor is not sufficient. Hash algorithm, seed, key
  order, post-cast types, null policy, transform semantics, and format version are part
  of correctness and belong in the first data model.
- Current partition counts are selected from runtime bytes. The static property needs
  a symbolic domain that resolves once and is shared by sibling exchanges and outputs.

A second, code-verified review (2026-07-02) confirmed every codebase claim in this
document, and this revision integrates its surviving findings: a grouping-sets gate on
aggregate reuse, an explicit cross-repository token transport in PR 1, the #1038
slot-coalescing overlap treated as a data-model decision rather than a sequencing
question, explicit join execution-mode and concat_all ownership under REUSE, a single
exchange-mode assignment for scan-side layouts, and defined semantics for transform,
format_version, column_index, and generation.

## Goals

- Avoid a redundant shuffle when an upstream Sirius operator already established a
  compatible distribution.
- Avoid scan-side shuffles for trusted pre-partitioned layouts.
- Decode or serve each source partition on the GPU that will consume it.
- Preserve the current hash-shuffle path as the fallback.
- Keep logical partition identity independent from active GPU count and physical slot.
- Propagate distribution through filters, direct projections, eligible INNER joins,
  and compatible local grouped aggregates.
- Make reuse decisions observable and testable.

## Non-Goals

- Replacing the current hash join or grouped-aggregate implementations.
- Removing CONCAT or MERGE_GROUP_BY in the first release.
- Treating file names or directory order as proof of partitioning.
- Supporting arbitrary computed partition expressions initially.
- Enabling reuse for outer, semi, anti, MARK, or mixed-condition joins initially.
- Enabling aggregate reuse for multiple grouping sets or grouping functions.
- Automatically correcting skew.
- Trusting optimizer cardinality estimates for correctness.

## Current Implementation

### Pipeline boundaries

[src/pipeline/sirius_pipeline_converter.cpp](../../src/pipeline/sirius_pipeline_converter.cpp)
unconditionally inserts PARTITION and CONCAT around hash-join inputs and inserts
PARTITION before MERGE_GROUP_BY:

~~~text
probe input -> PARTITION -> CONCAT -> HASH_JOIN
build input -> PARTITION -> CONCAT -> HASH_JOIN

input -> local HASH_GROUP_BY -> PARTITION -> MERGE_GROUP_BY
~~~

A third insertion site partitions DISTINCT-aggregate inputs into a distinct merge
pipeline (sirius_pipeline_converter.cpp:909-930). It hash-partitions on keys like the
other two sites and is covered by the same property model, but it is excluded from the
first reuse targets.

[src/planner/sirius_plan_aggregate.cpp](../../src/planner/sirius_plan_aggregate.cpp)
contains can_use_partitioned_aggregate(), but every branch creates the same physical
aggregate and pipeline conversion still inserts PARTITION. The check currently has no
execution effect.

### Runtime partition metadata

[src/include/op/sirius_physical_operator.hpp](../../src/include/op/sirius_physical_operator.hpp)
defines partitioned_operator_data with one partition index.
[src/creator/task_creator.cpp](../../src/creator/task_creator.cpp) maps that index to:

~~~text
active_gpu_ids[partition_idx % active_gpu_ids.size()]
~~~

This type does not identify keys, algorithm, partition count, or provenance. It is also
used for two different purposes:

- STANDARD/MIXED hash-join tasks carry a logical input partition index.
- BUILD_PROBE tasks carry the join operator ID so all tasks share one GPU-resident hash
  table.

[src/op/sirius_physical_hash_join.cpp](../../src/op/sirius_physical_hash_join.cpp)
returns pipelineable_operator_data from join execution, so the logical index does not
survive the join. Filter, projection, and local grouped aggregate have the same output
behavior. A new semantic token and an explicit propagation rule are required.

### Hash contract

[src/op/partition/gpu_partition_impl.cpp](../../src/op/partition/gpu_partition_impl.cpp)
uses cuDF Murmur3 with the cuDF default seed, ordered key indices, and per-key casts
when join key physical types differ. Equal SQL values encoded as different physical
types can hash differently. These details are part of distribution compatibility.

### Join execution mode and CONCAT folding

The sibling count-resolution step does more than pick a number. It is the sole call
site of hash_join.update_join_exec_mode(), which selects STANDARD, BUILD_PROBE, or
MIXED execution, and when BUILD_PROBE is chosen it mutates the downstream build-side
CONCAT via set_concat_all(true) (src/op/sirius_physical_partition.cpp:299-347).
concat_all is also set at construction from the join type: LEFT/ANTI/SEMI fold the
build CONCAT, RIGHT variants fold the probe CONCAT, FULL OUTER folds both
(src/op/sirius_physical_concat.cpp:41-63). Any exchange mode that skips byte-driven
count resolution must still make these decisions explicitly.

### Repository and empty partitions

The partition sink assigns each produced batch to a numbered repository partition.
Repositories grow from inserted batches, which does not represent trailing empty
partitions without an explicit expected count. The current hash partition path emits
typed empty batches, while a pre-partitioned source commonly omits empty buckets.

Hash join task creation expects equal partition counts on both sides. Outer joins with
multiple partitions are already tracked as incorrect in
[#485](https://github.com/sirius-db/sirius/issues/485), so the first release must be
limited to INNER equi-joins and grouped aggregates.

### Scan and pinned-cache metadata

The Parquet coalescer already separates files with different Hive partition values,
but scan splits do not carry a semantic logical partition. Pinned entries record
columns and memory spaces, but not a distribution specification, per-chunk logical
partition IDs, or a selected layout generation.

### Relationship to PR #1038

PR #1038 overlaps more than it complements:

- #1038 reduces the materialization and transfer cost when a shuffle occurs.
- This design proves when the shuffle can be bypassed.
- #1038's multi-GPU fast path coalesces fine hash partitions into per-GPU slot batches
  (concatenating every partition p with p modulo G equal to the slot) and sinks by
  slot. A coalesced batch spans many logical partitions and the repository index
  becomes the slot, so a one-ID-per-batch token cannot describe it, and downstream
  join tasks no longer see single-partition inputs.

This is a representational conflict, not a sequencing question. PR 1 must resolve it
explicitly: either partitioned_output_data allows a batch to carry a partition-ID
list (slot batches are then token-bearing but not reusable as single partitions), or
the coalescing fast path is disabled on exchanges that participate in reuse. Either
way, a physical slot must not replace the logical partition ID — several logical
partitions may map to one GPU slot — and the reuse tests must cover the coalescing
path.

## Preliminary Phase 0 Evidence

The parallel draft reported a TPC-H SF30 trace study on one NVIDIA GB10. It ran each of
the 22 queries twice, analyzed the warm run, and compared defaults with
hash_partition_bytes forced to 64 MiB.

Reported results:

- At defaults, 192 of 194 partition-count decisions selected one partition. The cause
  is structural: the build side's bytes decide the count for both join sides, and
  TPC-H build sides are nearly always under the 512 MB default target. PARTITION plus
  CONCAT represented 1.5% of measured operator time in that regime.
- Scans emitted 63 GB per warm benchmark pass, and 55 GB flowed through PARTITION.
  The byte volume is material even when one-partition execution makes kernel cost low.
- At 64 MiB, PARTITION plus CONCAT represented 11.2% of non-scan operator time.
  Including merge operators raised the share to 19.5%.
- Q18 was the clearest intra-query target. PARTITION, CONCAT, and merge operators
  were reported as 32.0% of the query's measured operator time in the 64 MiB regime,
  after a join partitioned by l_orderkey.

These numbers are directional, not an acceptance baseline. The raw trace logs,
task_outputs.csv files, exact configuration snapshot, and analyzed commit are not
present in this worktree. Before publishing a performance claim, archive those
artifacts and reproduce the run, including a multi-GPU run where transfer avoidance is
expected to dominate. The reproduction recipe (runner, configuration keys, and the
log-analyzer invocation) is retained in the measurement note linked in the header.

The evidence is still useful for sequencing: implement the lower-cost intra-query path
first, then evaluate pinned layouts on multi-GPU and memory-constrained workloads.
Pin-time bucketing also intersects #995 (it replaces the many-small-copies
partitioning phase with correctly placed I/O) and #400 (it decides the on-GPU input
organization those settings target).

## Distribution Model

### Semantic distribution specification

Introduce an immutable property shared by planning and execution:

~~~cpp
enum class distribution_kind {
  UNKNOWN,
  SINGLE,
  HASH,
  KEY_GROUPED
};

struct distribution_key {
  std::size_t column_index;
  partition_transform transform;
  sirius::logical_type result_type;
};

struct partition_count_spec {
  enum class mode { FIXED, RUNTIME };
  mode resolution;
  std::uint32_t fixed_value;
  std::uint64_t runtime_domain_id;
};

struct distribution_spec {
  distribution_kind kind;
  std::vector<distribution_key> keys;
  hash_algorithm algorithm;
  std::uint32_t seed;
  partition_count_spec partitions;
  null_policy nulls;
  std::uint32_t format_version;
};

struct resolved_distribution_spec {
  distribution_spec descriptor;
  std::uint32_t partition_count;
};

~~~

Current SHUFFLE counts are selected from runtime bytes, so a planning property may
carry a symbolic RUNTIME domain rather than a number. Sibling join exchanges and the
join output refer to the same domain ID. The build-side decision resolves it once into
a resolved_distribution_spec shared by all runtime tokens. Pinned layouts use FIXED
domains whose count is known during planning.

The column indices are relative to the producing operator's output. Semantic equality
must compare every correctness-relevant field. A cache entry ID is useful provenance,
but object identity cannot replace semantic comparison.

For an external transform, algorithm identifies both the hash and value-encoding
rules. An Iceberg bucket is not assumed compatible with cuDF Murmur3 merely because
both are described as buckets.

Field semantics:

- transform (per key) is the value mapping applied to that key before hashing:
  IDENTITY for the MVP, later values such as BUCKET or DAY. A required cast is
  expressed through the transform's result_type. algorithm (per spec) is the
  row-level hash and its value-encoding rules applied to the transformed key vector.
  An external layout is therefore modeled as transformed keys plus a named algorithm
  (for example ICEBERG_BUCKET_V1), never as an anonymous bucket transform with an
  unspecified hash.
- format_version versions the hash-and-encoding implementation contract. It must be
  bumped whenever the produced bucket assignment could change for identical input
  (for example a cuDF Murmur3 or encoding change). A version mismatch is a
  compatibility failure and forces SHUFFLE. Serialization-schema evolution of the
  spec struct itself is a separate storage concern and must not reuse this field.
- column_index is an ordinal in the producing operator's output only for intra-query
  properties. A spec persisted on a pinned entry stores key identity relative to the
  pinned entry's column list; the scan bind step remaps it to query-time scan output
  ordinals and fails closed to no-property when a key is not projected.
- generation is a monotonically increasing identity for a pinned entry's contents.
  Re-pinning under the same name creates a new generation and never mutates a bound
  one.

SINGLE is the normalized one-partition property. It satisfies any co-location
requirement when all relevant inputs are also single-partition, although cost policy
may reject it for large data. KEY_GROUPED is reserved for external identity layouts;
it must define a canonical value-to-logical-ID mapping before it is enabled.

### Runtime partition token

Each batch that is proven to belong to one logical partition carries:

~~~cpp
struct partition_token {
  std::shared_ptr<const resolved_distribution_spec> spec;
  std::uint32_t logical_partition_id;
};
~~~

The ID must be less than spec->partition_count. The token is meaningful only with
its spec.

Do not overload this token for generic GPU affinity. BUILD_PROBE's same-GPU constraint
should use a separate affinity key or preferred-device mechanism. Scheduling can use a
semantic token as one source of affinity, but affinity alone never proves distribution.

### Multi-output partition data

A shuffle task produces several logical partitions. Replace positional assumptions
with IDs parallel to the output batches:

~~~cpp
struct partitioned_output_data : pipelineable_operator_data {
  std::vector<std::uint32_t> logical_partition_ids;
  std::shared_ptr<const resolved_distribution_spec> spec;
};
~~~

PR #1038 may additionally carry a physical target slot. Logical IDs remain the
repository keys and must survive changes in active GPU count.

### Static property and runtime proof

The physical plan carries a provided distribution property. Runtime operator data
carries the proof for one batch. Both are required:

- The planner uses the property to choose an exchange mode.
- The exchange validates runtime tokens before forwarding.
- Operators propagate a token only when their planned rule proves preservation.
- SHUFFLE creates a new spec and tokens.

## Physical Property Propagation

Run a distribution-property pass before pipeline splitting. Each physical operator
describes how its output property is derived.

For a join whose partition count is not known until execution, the pass creates one
symbolic runtime domain for both input requirements and the eligible join output. The
pipeline converter binds inserted PARTITION operators to that domain; runtime count
resolution then flows through tokens without requiring a second planning pass.

| Operator | Initial rule |
|---|---|
| TABLE_SCAN / GPU_SCAN | Property of the selected pinned or external layout |
| FILTER | Preserve |
| STREAMING_LIMIT | Preserve remaining rows |
| PROJECTION | Remap direct references; clear if a key is dropped or computed |
| HASH_GROUP_BY | Preserve only for plain aggregates (single grouping set, no grouping functions) whose GROUP BY includes the input keys; remap to grouping output |
| INNER HASH_JOIN | Preserve a validated input join distribution when an equivalent output key survives |
| Other joins | Clear |
| ORDER_BY / SORT_PARTITION | Clear hash property; range distribution is separate future work |
| PARTITION(SHUFFLE) | Establish configured hash distribution |
| PARTITION(REUSE) | Preserve validated input distribution |
| CONCAT (not concat_all) | Preserve one logical partition per output task |
| CONCAT (concat_all) | Collapse to SINGLE; the folded output spans partitions |
| MERGE_GROUP_BY | Consume partitioned partials; output may retain the exchange's input spec, never a widened key set |
| Any operator not listed | UNKNOWN (clear): delim joins, CTE, COLUMN_DATA_SCAN, CPU_SOURCE, TOP_N, UNION, the other merge operators |

Only direct-reference projections are supported initially. A cast is compatible only
when the cast and target type are part of the distribution key transform.

The pass must default every unlisted operator to UNKNOWN. Delim joins need special
care: the internal comparison join is a member of the delim operator, not a plan-tree
child (the original children are rewired to a dummy scan or a runtime
COLUMN_DATA_SCAN, src/op/sirius_physical_delim_join.cpp:74-104), so a tree pass never
visits it, while the pipeline converter still manufactures partition_join and
partition_distinct exchanges for it (sirius_pipeline_converter.cpp:823-931). The
initial release leaves every delim-internal exchange in SHUFFLE mode, and
exchange-mode additions must be proven inert for that shape.

### INNER hash-join output

When both join inputs are partitioned compatibly on equality keys, every matching row
pair is produced in one logical partition. The output can therefore preserve that
distribution if:

- The join is INNER and contains only supported equality conditions.
- Input specs are compatible with the corresponding equality keys.
- At least one equivalent key for every distribution key survives the join projection.
- Output key ordinals and post-cast types can be mapped unambiguously.
- The task input carries a semantic token established by SHUFFLE or REUSE; a
  BUILD_PROBE affinity-only tag does not qualify.

The runtime output token uses the logical partition ID from the join task input and the
remapped output spec. If any key is omitted or computed, output distribution is
UNKNOWN. Semi, anti, outer, MARK, and mixed-condition propagation are deferred.

### Grouped aggregate compatibility

SINGLE satisfies any grouped-aggregate co-location requirement when accepted by cost
policy. For a multi-partition HASH input:

HASH(Kin) satisfies GROUP BY Kgroup when every key in Kin is included in Kgroup with
compatible type and transform. Equal complete group keys then cannot occur in
different logical partitions.

The rule applies only to plain aggregates: a single grouping set and no grouping
functions. DuckDB lowers GROUPING SETS, ROLLUP, and CUBE into one aggregate whose
group list is the union of all sets, and Sirius forwards such plans into the grouped
aggregate without rejection. Any set that omits a distribution key (for example the
rollup grand total) makes equal complete keys occur in several logical partitions,
and MERGE_GROUP_BY, which merges strictly within one partition, would emit one
subtotal row per partition. Gate reuse with the same predicate
can_use_partitioned_aggregate already applies: grouping_sets.size() > 1 or any
grouping functions means SHUFFLE (src/planner/sirius_plan_aggregate.cpp:137; the
partition-key derivation TODO at src/op/sirius_physical_partition.cpp:120-130
documents the same hole).

| Input distribution | GROUP BY | Reusable |
|---|---|---|
| HASH(a) | a | Yes |
| HASH(a) | a, b | Yes |
| HASH(a, b) | a, b | Yes |
| HASH(a, b) | a | No |
| HASH(a) | expression(a) | No initially |

MERGE_GROUP_BY remains necessary because several batches in one logical partition can
contain partial results for the same group.

### Join-input compatibility

Two existing layouts can feed a partitioned INNER join when:

- Both are compatible HASH distributions, or both resolve to SINGLE.
- Ordered key counts and equality-key mappings match.
- Fixed counts match, or both inputs share one runtime domain and resolved count.
- For HASH, algorithm, seed, format version, post-transform key types, and null
  policy match.
- Intervening operators preserved the keys.
- Both join exchanges make one atomic reuse decision.

The first implementation should require exact key vectors. A later rule may prove that
partitioning by a subset of equality keys is sufficient.

### One-sided reuse

One trusted Sirius layout can be reused while the other side shuffles when Sirius can
compute the exact same transform:

~~~text
pinned side -> PARTITION(REUSE, N)   -> CONCAT[N] -> HASH_JOIN[N]
other side  -> PARTITION(SHUFFLE, N) -> CONCAT[N] -> HASH_JOIN[N]
~~~

The reused spec is authoritative and fixes N, algorithm, seed, key order, casts, and
null policy for the other exchange. This is not allowed for an external transform that
Sirius cannot reproduce. In that case both sides must already share the external spec,
or both must use a new Sirius shuffle.

### Correctness and cost are separate

Use two checks:

~~~text
distribution_satisfies(requirement)
reuse_is_desirable(partition_stats, active_gpus, operator_params)
~~~

Correctness uses only the semantic spec. Cost policy may reject a correct layout due to
under-parallelism, oversized partitions, skew, or task overhead.

For intra-query reuse, inspect actual bytes per logical partition at the existing
exchange barrier. Do not compare only total bytes and do not depend on optimizer
estimates. If the largest partition violates the downstream memory policy, choose
SHUFFLE before forwarding any batch. Downgrade/OOM handling remains the final safety
net, not the reuse selection mechanism.

The initial policy: accept reuse when the maximum actual partition bytes are at most
hash_partition_bytes times reuse_partition_tolerance (see Configuration Surface), and
on multi-GPU when the resolved count meets the min_num_partitions floor for
above-threshold inputs; otherwise SHUFFLE. Partition refinement — re-hash-splitting
only the oversized partitions under the same spec instead of a full reshuffle — is
the later alternative when a layout is almost right.

## Exchange and Pipeline Design

### Exchange modes

Extend sirius_physical_partition with:

~~~cpp
enum class exchange_mode {
  SHUFFLE,
  REUSE,
  REUSE_OR_SHUFFLE
};
~~~

Renaming PARTITION to EXCHANGE is optional and should not be mixed into the first
implementation.

In REUSE mode:

- The input token is required and validated against the planned spec. A missing or
  invalid token here is a hard invariant failure, not a silent fallback.
- execute forwards the batch without hashing, reordering, or cloning where possible.
- sink publishes the batch under token.logical_partition_id rather than a local loop
  counter.
- The repository is initialized to spec.partition_count. This is a count
  representation: expected empty partitions are represented, never materialized as
  empty tables.
- When the exchange feeds a join, it still drives join execution-mode selection and
  CONCAT folding: update_join_exec_mode runs with the spec's resolved count and the
  bytes observed at the barrier, and concat_all configuration remains this step's
  responsibility even though no hash kernel runs.
- Downstream CONCAT and MERGE operators keep their existing roles.

REUSE is appropriate for a distribution established earlier in the same query, where
the token's presence is an engine invariant. REUSE_OR_SHUFFLE is appropriate when a
selected cache generation or source promise can disappear between planning and
execution; only those source-layout exchanges degrade silently.

### Phase 1 intra-query path

Current shape:

~~~text
join partition p
  -> INNER HASH_JOIN
  -> filter/direct projection
  -> local HASH_GROUP_BY
  -> PARTITION(SHUFFLE)
  -> MERGE_GROUP_BY
~~~

Initial reused shape:

~~~text
join partition p
  -> INNER HASH_JOIN[token p]
  -> filter/direct projection[token p]
  -> local HASH_GROUP_BY[token p, remapped keys]
  -> PARTITION(REUSE p)
  -> MERGE_GROUP_BY[p]
~~~

The first end-to-end query shape should be Q18-like join-to-group-by reuse. Chained
same-key INNER joins follow after output-key remapping is covered.

Do not initially wire the upstream repository directly to MERGE_GROUP_BY. Keeping
PARTITION(REUSE) provides a place to validate specs, initialize empty partitions,
apply the memory policy, log the decision, and fall back before publication.

### Source join path

Both sides reusable:

~~~text
probe scan[p] -> PARTITION(REUSE_OR_SHUFFLE p) -> CONCAT[p] -> HASH_JOIN[p]
build scan[p] -> PARTITION(REUSE_OR_SHUFFLE p) -> CONCAT[p] -> HASH_JOIN[p]
~~~

One side reusable:

~~~text
probe scan[p] -> PARTITION(REUSE_OR_SHUFFLE p, fixed spec) -> CONCAT[p] -> HASH_JOIN[p]
build input   -> PARTITION(SHUFFLE, fixed spec)            -> CONCAT[p] -> HASH_JOIN[p]
~~~

Scan-side exchanges always use REUSE_OR_SHUFFLE because a bound layout generation can
disappear between planning and execution; REUSE with its hard-failure contract is
reserved for distributions established earlier in the same query.

Join-side decisions are coordinated. A runtime validation failure must switch the
whole pair to a compatible plan before either exchange publishes output. Mid-stream
fallback is not valid. The fixed-count SHUFFLE path bypasses byte-driven sibling
resolution, which is also where join execution mode and concat_all are configured
today; it must run that configuration step explicitly with the spec's count, and must
respect or deliberately override the set_min_num_partitions multi-GPU floor
(src/op/sirius_physical_partition.cpp:246-255).

### Central token propagation

run_one_operator should derive output metadata from the planned operator rule:

- Preserve and remap the token for eligible filter, projection, join, and local
  grouped-aggregate outputs.
- Clear it for operators whose rule is UNKNOWN.
- Replace it for PARTITION(SHUFFLE).
- Never manufacture a semantic token from preferred_device_id or an affinity key.

Central propagation avoids duplicating metadata plumbing in every operator, while the
operator-specific property rule remains explicit and testable.

run_one_operator only covers hops inside one task. Tokens die at pipeline boundaries:
batches cross them through repositories as raw cucascade::data_batch values, which
carry no metadata slot, and the receiving operator's task input is reconstructed as
plain pipelineable_operator_data (src/op/sirius_physical_operator.cpp:238-261,
316-330). PR 1 must therefore add a token transport across repositories. Options, in
preference order: (a) upstream sinks feeding a REUSE exchange publish partitioned
(push_data_batch_partitioned), so the repository partition index itself carries the
identity; (b) a Sirius-side batch-ID-to-token map with query lifetime; (c) a metadata
slot on cucascade::data_batch (a submodule API change). The Phase 1 shape needs this
at exactly one boundary — the local HASH_GROUP_BY sink into the aggregate exchange —
which favors option (a).

### Repository and scheduling

Add an operation equivalent to:

~~~cpp
repo.ensure_num_partitions(spec.partition_count);
~~~

idata_repository lives in the cucascade submodule (add_data_batch auto-resizes on
insert), so this is a cross-repository change. The expected count is also a property
of the downstream chain, not of one repository: today every shuffle task emits all N
batches, including typed empty ones, which implicitly sizes the CONCAT output
repository and the join port repositories that the join's count-equality check reads
(src/op/sirius_physical_hash_join.cpp:574-589). Under reuse without empty batches,
the count must be applied to the exchange's consumer repository and forwarded through
CONCAT's sink to the join and merge port repositories.

Repository partition indices are logical IDs. Scheduling derives:

~~~text
physical_slot = logical_partition_id % active_gpu_ids.size()
~~~

The scan balancer, pinned placement, CONCAT tasks, merge tasks, and join tasks must use
the same mapping. A separate affinity key handles BUILD_PROBE's single-GPU hash table.

### Empty partitions and join gating

Pre-partitioned storage generally omits empty buckets. The runtime must still represent
the full logical domain.

- INNER joins can skip work when either side of a partition is empty.
- Grouped aggregate merge can skip an empty partition.
- LEFT, RIGHT, FULL, SEMI, and ANTI forms need explicit missing-side semantics before
  reuse is enabled.

Do not materialize empty tables solely to make a partition count visible. Teach
repository and task creation to represent an expected empty partition explicitly.

## Sirius-Created Pinned Layouts

### API

Extend pin_table with explicit bucket semantics:

~~~sql
CALL pin_table(
  'orders/*.parquet',
  tier => 'gpu',
  name => 'orders_by_orderkey',
  bucket_by => ['o_orderkey'],
  buckets => 64
);
~~~

bucket_by is preferred to a generic partition_by name because this operation hashes
rows into a fixed bucket domain rather than preserving directory partitions.

### Pin-time behavior

1. Read requested columns and every bucket key.
2. Hash with the same versioned implementation used by runtime SHUFFLE.
3. Store each output chunk with its logical bucket ID.
4. Place bucket p with the GPU selected by p modulo the active GPU count.
5. Record the immutable spec, per-chunk IDs, entry generation, and source identity.
6. For host tier, use the NUMA-local host space paired with the target GPU.

Initially require bucket_by to be included in cols when cols is supplied. Hidden
bucket-key retention can be designed later.

### Multiple layouts

One table may be useful under several keys. Planning must enumerate cache candidates
by source identity and column coverage, choose a layout satisfying the downstream
requirement, and bind the exact pinned-entry ID and generation to the scan. Execution
must not silently select the first cache entry that happens to cover the columns.

If a bound generation is unavailable at bind time, planning selects another layout or
SHUFFLE. Sirius has no mid-query replanning: once execution starts, the only valid
degradation is the exchange's REUSE_OR_SHUFFLE fallback, taken before any source task
publishes data.

### Residency, downgrade, and generation lifetime

Bucket placement (p modulo the active GPU count) is a locality preference, not a
correctness property: a partition_token stays valid wherever its batch physically
lives, including after GPU-to-HOST-to-DISK downgrades and upgrades to a different
GPU. The implementation must still answer two policy questions explicitly: whether
bucketed pinned chunks are downgrade candidates at all (and whether an upgrade
restores them to the owning GPU), and how memory accounting charges multiple bucketed
generations of the same table.

A running query must hold a reference to its bound generation. pinned_entry today has
no generation field, and remove_pinned_entry can drop an entry at any time
(src/include/scan_manager/sirius_scan_manager.hpp:121-147), so PR 3 adds the
generation field and a query-scoped pin on the bound generation.

### Scan metadata

- parquet_file_scan_info carries an optional logical partition ID and spec identity.
- The Parquet coalescer flushes when that identity changes.
- A split carries exactly one logical partition ID.
- The load-balancing coalescer derives the preferred GPU from the logical ID.
- The GPU scan emits a partition token with decoded batches.

Fresh scans then decode on the GPU that will consume their join or aggregate partition.

### External layouts

Add format adapters after Sirius-created layouts:

- Iceberg: parse partition specs and manifest partition tuples; begin with identity and
  bucket transforms whose exact encoding is implemented.
- Hive-style Parquet: use identity partition values as KEY_GROUPED when appropriate.
- Raw Parquet: require a versioned sidecar manifest defining the spec and file mapping.

File names, directory order, and optimizer claims are not proof. If both join inputs
share an external spec, Sirius may reuse it without reproducing the transform. One-sided
reuse requires Sirius to implement that exact transform.

## Compatibility Matrix

| Situation | Initial action |
|---|---|
| Join output distribution satisfies downstream GROUP BY | REUSE aggregate exchange |
| Both join inputs resolve to SINGLE | Reuse the one-partition path if cost policy accepts it |
| Both join sides have exact compatible Sirius layouts | REUSE_OR_SHUFFLE both |
| One side has a compatible Sirius layout | REUSE_OR_SHUFFLE it; SHUFFLE the other to its fixed spec |
| Both sides have the same supported external layout | REUSE both after adapter support |
| Only one side has an unreproducible external layout | SHUFFLE both with Sirius |
| Partition counts, algorithm, seed, version, or null policy differ | SHUFFLE both |
| Join requires a key cast not encoded in the layout | SHUFFLE both |
| Filter lies between producer and consumer | Preserve |
| Direct projection reorders keys | Remap |
| Projection drops or computes a key | Clear and SHUFFLE |
| GROUP BY contains all distribution keys | REUSE |
| GROUP BY omits a distribution key | SHUFFLE |
| Aggregate has multiple grouping sets or grouping functions | SHUFFLE |
| Correct layout has an oversized partition or too little parallelism | SHUFFLE by policy |
| Bound pinned generation disappears | Replan, coordinated fallback, or safe error |
| Join type is not INNER equi-join | SHUFFLE/current path; no reuse initially |

## Configuration Surface

- enable_exchange_reuse (operator_params, default off until PR 2 acceptance): the
  global kill switch. When off, every exchange plans SHUFFLE exactly as today.
- reuse_partition_tolerance (operator_params, default 2.0): the cost-policy bound.
  Reuse only when the maximum actual partition bytes are at most
  hash_partition_bytes times this tolerance.
- pin_table requires an explicit buckets count in PR 3. An automatic default derived
  from table bytes, hash_partition_bytes, and GPU count is deferred (open question).
- Counters (hash-partition kernel invocations, reuse decisions and rejection reasons,
  bytes forwarded versus shuffled) are exposed through the existing telemetry and log
  stream; PR 2's zero-second-kernel acceptance test reads the kernel counter. EXPLAIN
  integration is deferred to the synthetic-operator explanation work.

## Observability

Every exchange should report:

- Mode and whether fallback occurred.
- Distribution kind, key ordinals, post-transform types, algorithm, seed, and version.
- Logical partition count and active GPU count.
- Property provenance: upstream exchange, join output, or pinned layout generation.
- Reuse rejection reason.
- Input bytes per logical partition and maximum partition bytes.
- Hash-partition kernel invocation count.
- Bytes copied within and across GPUs.

Example debug output:

~~~text
PARTITION[mode=REUSE, keys=(#0), partitions=64, source=join:42]
PARTITION[mode=SHUFFLE, keys=(#0), partitions=8, reason=max_partition_bytes]
~~~

## Rollout Plan

### Phase 0: preserve evidence

- Check in or otherwise archive the benchmark command, commit SHA, hardware details,
  config, raw trace logs, and generated task_outputs.csv files.
- Reproduce the reported single-GPU numbers.
- Add a multi-GPU baseline before claiming transfer savings.

### PR 1: distribution and affinity contracts

- Add distribution_spec compatibility and mismatch reasons.
- Add fixed and symbolic runtime partition domains with one-time count resolution.
- Add partition_token without changing plan selection.
- Separate semantic logical partition from generic execution affinity.
- Add explicit logical IDs for multi-output partition data.
- Add expected repository partition count and empty-partition representation,
  propagated through the downstream repository chain.
- Add property rules and token remapping for filter and direct projection.
- Choose and implement the cross-repository token transport (see Central token
  propagation).
- Resolve the #1038 slot-coalescing representation conflict: multi-ID batches or a
  reuse-path opt-out from coalescing.

### PR 2: intra-query aggregate reuse

- Propagate a compatible distribution through INNER equi-join output.
- Propagate and remap it through local HASH_GROUP_BY.
- Add PARTITION(REUSE) for grouped aggregate input.
- Add actual per-partition cost-policy checks (reuse_partition_tolerance).
- Gate aggregate reuse on a single grouping set and no grouping functions.
- Enable only join-to-group-by shapes initially.
- Validate Q18-like queries against DuckDB CPU results and confirm zero second
  hash-partition kernel invocations. The kernel-count assertion applies to plans
  without DISTINCT aggregates, whose separate exchange site is out of scope here.

### PR 3: pinned bucket layouts

- Add bucket_by and buckets to pin_table.
- Partition once at pin time.
- Store spec, logical IDs, source identity, and generation in pinned_entry.
- Make cache selection distribution-aware and deterministic.
- Route GPU and host chunks by logical partition.
- Add the pinned_entry generation field and query-scoped generation pinning.

### PR 4: source join reuse

- Add coordinated sibling decisions.
- Reuse exact compatible layouts on both sides.
- Add fixed-spec SHUFFLE for one-sided Sirius layout reuse, including explicit join
  execution-mode and concat_all configuration and multi-GPU floor handling on the
  fixed-count path.
- Keep CONCAT and current join task creation.
- Add scan-side REUSE_OR_SHUFFLE and generation validation.

### PR 5: external adapters and broader propagation

- Add Hive identity and Iceberg identity/bucket adapters.
- Add a raw-Parquet sidecar only if needed.
- Consider chained INNER joins, compatible partition-count coalescing, and additional
  join forms after correctness coverage.

## Test Plan

### Unit tests

- distribution_spec equality and a distinct rejection reason for every field.
- Fixed-count compatibility and shared runtime-domain resolution.
- Distinction between logical partition tokens and BUILD_PROBE affinity.
- Projection and join-output key remapping.
- Aggregate subset rule.
- One-sided fixed-spec join planning.
- Runtime token validation and atomic fallback selection.
- Leading, middle, and trailing empty partitions.
- Cost policy based on maximum actual partition bytes.

### Operator tests

- PARTITION(REUSE) invokes no hash kernel and preserves batch identity where possible.
- PARTITION(SHUFFLE) remains behaviorally unchanged.
- REUSE sink publishes by token ID, not task-local batch order.
- CONCAT (not concat_all) combines only one logical partition; the #1038 coalescing
  path is exercised separately.
- Join output preserves a token only for eligible INNER equi-joins.
- Local grouped aggregate remaps a compatible token to grouping output columns.
- Sibling join exchanges choose reuse or fallback atomically.

### Integration tests

Cover:

- Q18-like join to grouped aggregate with no second shuffle.
- Chained same-key INNER joins after the first target is stable.
- Duplicate and null keys.
- Empty logical buckets.
- Several batches per bucket.
- Filters and direct projections.
- Dropped and computed partition keys.
- GROUPING SETS, ROLLUP, and CUBE queries never take aggregate reuse.
- Mismatched counts, seeds, algorithms, versions, key order, and key types.
- One-sided pinned layout reuse.
- Cache generation replacement between planning and execution.

Compare every result with DuckDB CPU execution. Add negative tests proving that an
incompatible property uses SHUFFLE.

### Multi-GPU tests

- Every task for logical partition p uses active_gpu_ids[p modulo G], except
  BUILD_PROBE tasks, which pin to one GPU via their separate affinity key.
- BUILD_PROBE affinity does not create a semantic partition token.
- Fresh scans decode on the target GPU.
- Pinned GPU scans avoid peer copies before eligible consumers.
- Host-tier chunks use the NUMA region paired with the target GPU.
- Changing the active GPU count preserves correctness.
- Downgrade pressure during a reused query: tokens stay valid across tier moves and
  results are unchanged.

### Performance validation

Use Q18 for intra-query reuse. Use Q12 and Q14/Q19 for bucketed source joins. Measure:

- End-to-end and non-scan operator time.
- PARTITION and CONCAT time.
- Hash-partition kernel count.
- Bytes allocated and materialized by PARTITION.
- D2D and host-staged cross-GPU bytes.
- Maximum bytes per logical partition.
- Pin-time preprocessing and repeated-query break-even point.

The first acceptance criterion is correctness plus elimination of the eligible hash
kernel and data movement. Set a speedup threshold only after reproducible baselines.

## Risks and Tradeoffs

### Metadata is correctness-bearing

Incorrect metadata can silently omit join matches. Specs are versioned and validated,
and source metadata is treated as proof rather than a cost hint.

### Join output mapping is subtle

Join projection maps, duplicate key columns, post-cast types, and missing output keys
can invalidate propagation. Start with an exact INNER equi-join rule and clear the
property on ambiguity.

### Reuse can violate a downstream cost target

An upstream count was selected for a different data volume. A many-to-many join can
expand a partition. Inspect actual per-partition bytes and retain SHUFFLE as a policy
fallback.

### Fixed layouts trade memory for reuse

Tables joining on several keys may need several pinned layouts. Layout selection must
be deterministic, and memory accounting must include every generation.

### Skew remains

Pre-partitioning removes repeated movement but does not balance a hot key. Cost policy
may reject a skewed layout.

### Benchmark evidence is not yet auditable

The reported Phase 0 summary is useful directionally, but raw artifacts must be retained
before using exact percentages in project decisions or release claims.

### External format evolution

Iceberg files may use several partition specs. Reuse is valid only when selected files
have one compatible execution distribution or the adapter explicitly reconciles them.

## Open Questions

- Should bucketed layouts remain a pin_table option or become a separate materialized
  layout API?
- What target bytes, GPU count, and expected reuse should choose the default bucket
  count?
- Should bucket keys be retained as hidden columns when cols omits them?
- How should cost policy balance an oversized logical partition against the movement
  required to reshuffle it? Partition refinement (re-hash-splitting only the oversized
  partitions under the same spec) is the leading candidate.

## Recommendation

Implement the semantic distribution and affinity split first, then deliver
join-output-to-group-by reuse behind PARTITION(REUSE). This proves the hard correctness
and runtime-plumbing pieces without a storage API and targets the strongest reported
single-GPU example.

Next, add Sirius-created bucketed pinned layouts, followed by two-sided and one-sided
source join reuse. Keep PARTITION as the validation and fallback boundary, retain
CONCAT and merge operators, and defer external layouts and non-INNER joins until the
exact compatibility rules and empty-partition behavior are covered.
