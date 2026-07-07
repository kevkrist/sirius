# Issue #1018 Implementation Handoff

**Status:** Design complete; production implementation not started<br>
**Issue:** [#1018 - Explore avoiding shuffles with pre-partitioned data](https://github.com/sirius-db/sirius/issues/1018)<br>
**Code-reading baseline:** `dev` at `c559d45cfd06043d2a4860e03501348547d940d5`

## Minimum Handoff Set

Read these issue-specific documents in order:

1. [Shuffle Elision with Proven Distribution](issue-1018-prepartitioned-shuffle-design.md)
   is the canonical architecture. It defines scope, correctness rules, compatibility,
   exchange modes, rollout order, and known risks.
2. [Distribution-Aware Exchange Software Design](issue-1018-software-design.md)
   is the implementation design. It defines modules, value types, ownership, runtime
   state, repository channels, pinned-layout leases, concurrency, tests, and staged
   file changes.
3. [Phase 0 Measurement Note](issue-1018-shuffle-elision-design.md) preserves the
   original measurements and rationale for sequencing. It is superseded for design
   decisions and should be read as evidence only.

The first two documents are required. The Phase 0 note is required before changing
performance priorities or publishing performance claims.

## Existing System Context

Before changing Super Sirius, read the repository instructions in
[CLAUDE.md](../../CLAUDE.md)
and the following existing documentation:

1. [Physical Plan Generation](physical-plan-generation.md)
2. [Pipeline Execution](pipeline-execution.md)
3. [Data Management](data-management.md)
4. [Task Creator](task-creator.md)
5. [Scan](scan.md)
6. [Multi-GPU Architecture](multi-gpu-architecture.md)
7. [Memory Management](memory-management.md)

These documents explain the code boundaries that the design changes. Update any stale
file or line references if implementation starts from a newer `dev` commit.

## Current State

- Research, code inspection, architecture, and implementation design are complete.
- No production code for issue #1018 has been implemented.
- No feature flag, distribution model, token transport, or reuse path exists yet.
- The current hash SHUFFLE path remains the required correctness fallback.
- The design was checked against the Super Sirius planner, pipeline converter,
  operator-data hierarchy, task creator, repositories, hash join, grouped aggregate,
  scan manager, and pinned-entry ownership.

The important settled decisions are:

- Distribution is a correctness-bearing semantic property, not a GPU-affinity hint.
- Logical partition identity and BUILD_PROBE affinity use separate types and paths.
- PARTITION and CONCAT remain in the initial DAG.
- Runtime partition domains and join coordination are query-scoped; there is no global
  registry or raw sibling coordination.
- Tokens cross pipeline barriers through explicit logical-partition repository
  channels.
- Repositories represent the complete logical count without dummy empty batches.
- Source layouts use immutable generations and query-lifetime leases.
- Exchanges participating in reuse disable PR #1038 slot coalescing until a truthful
  multi-partition batch representation is designed.
- Initial reuse is limited to eligible INNER equi-join output feeding a compatible
  grouped aggregate.
- Grouping sets, delim-internal exchanges, unsupported joins, ambiguous mappings, and
  unknown properties remain on SHUFFLE.

## Where To Resume

Begin with Stage 0 and Stage 1 from the software design. The first implementation PR
must preserve behavior and keep every exchange `shuffle_only`:

1. Add characterization tests for current partition counts, join mode, CONCAT,
   repository partitions, empty partitions, and OOM retry behavior.
2. Add a test/telemetry counter for hash-partition kernel invocations.
3. Implement the CPU-only distribution value model, exact equality,
   `distribution_satisfies`, typed mismatch reasons, and runtime-domain
   resolution.
4. Separate semantic partition metadata from generic execution affinity.
5. Add aligned batch metadata and central propagation plumbing, with every original
   operator rule defaulting to UNKNOWN/clear.
6. Add explicit distribution-channel wiring and
   `ensure_num_partitions()`, including leading, middle, trailing, and
   all-empty partition tests.
7. Migrate existing SHUFFLE and CONCAT behavior onto the new contracts without
   enabling reuse.

Only after that behavior-preserving foundation passes should the analyzer run in
shadow mode. The first enabled optimization is the Q18-like join-to-grouped-aggregate
path. Pinned bucket layouts and source-join reuse come later.

## Required Validation

Use the repository commands:

~~~bash
pixi run make test
pixi run pre-commit run -a
~~~

For focused development, add CPU-only Catch2 tests under
`test/cpp/distribution/` and operator/pipeline tests in the locations listed
by the software design. Before publishing a speedup claim, reproduce Phase 0 with the
raw logs, exact configuration, commit SHA, and multi-GPU hardware details retained.

## Constraints

- Modify Super Sirius under `src/`; do not implement this in
  `src/legacy/`.
- Treat UNKNOWN as SHUFFLE.
- Do not infer semantic distribution from repository order, output vector position,
  preferred device, memory space, or a raw partition integer without a channel
  contract.
- Do not change a coordinated exchange decision after either side publishes data.
- Do not commit the local copy of `C++ Software Design.pdf`. The software
  design links to the publisher page and summarizes only the applicable guidelines.

## No Open Blocker For Stage 1

The architecture document retains product and later-phase questions, but none blocks
the behavior-preserving Stage 1 contracts. If implementation uncovers a conflict with
the stated correctness invariants, update the architecture decision first rather than
silently weakening the runtime validation.
