# Legacy scan-executor removal — WIP progress doc

This branch is a multi-phase cleanup that deletes the legacy CPU-managed scan
path and refactors Iceberg onto the new GPU-native `sirius_gpu_parquet_scan`
operator.  This doc captures the approved plan and the current progress so the
work can be picked up on another machine.

**Delete this file before merging.**

---

## Context

Sirius has two coexisting scan worlds.

**Legacy (to delete entirely):**

- `parquet_scan_task` — host-side parquet reader emitting
  `host_parquet_representation`, host→GPU convert
- `duckdb_scan_task` — CPU-managed seq_scan path
- `iceberg_scan_task` — inherits `parquet_scan_task_global_state`, layers a
  GPU delete-filter pipeline on top
- `duckdb_scan_executor` — thread pool that schedules all three

**New (keep + extend):**

- `sirius_gpu_parquet_scan_operator` (used by `read_parquet`, `parquet_scan`,
  `sirius_read_parquet`)
- `sirius_gpu_duckdb_native_scan_operator` (used by `seq_scan`)
- Polymorphic `scan_info` subclasses (`parquet_scan_info`,
  `duckdb_native_scan_info`) dispatched to format-specific split providers by
  `sirius_scan_manager`

Goal: delete the legacy world entirely, refactor Iceberg onto
`sirius_gpu_parquet_scan` via a new `iceberg_scan_info` + `iceberg_split_provider`
that injects a per-batch GPU delete-filter hook into each emitted
`parquet_scan_data`.  Existing `iceberg_delete_filter` / `iceberg_delete_pipeline`
machinery is already cudf-based and is reused unchanged.  The
`enable_gpu_duckdb_native_scan` toggle is removed; `seq_scan` always routes to
the GPU-native path.  Engine-time iceberg metadata loading
(`prefetch_iceberg_delete_data` + `iceberg_delete_data_cache_`) is preserved.

---

## Phase 1 — DONE (additive iceberg refactor)

Built cleanly (`pixi run make`, exit 0).  Iceberg integration tests not yet
run — they require a GPU-equipped machine.

### Files added

| Path | Purpose |
|---|---|
| `src/include/op/scan/post_convert_fn.hpp` | `scan_post_decode_hook_t` typedef (renamed from legacy `post_convert_fn_t`); kept under the legacy filename so the moved import path is obvious. |
| `src/include/op/scan/iceberg_scan_info.hpp` + `src/op/scan/iceberg_scan_info.cpp` | `parquet_scan_info` subclass carrying `delete_data`; `make_provider()` builds an `iceberg_split_provider`. |
| `src/include/scan_manager/iceberg_split_provider.hpp` + `src/scan_manager/iceberg_split_provider.cpp` | Subclass of `parquet_split_provider`.  Widens `projection_ids` with eq-delete keys, reads the first data file's footer once to build a field-id map, composes the existing `iceberg_delete_pipeline`, forces `max_file_processed = 1` for clean positional-delete `first_row` semantics, and wraps the base's `next_split_provider` to attach the hook to every emitted `parquet_scan_data`. |

### Files modified

| Path | Change |
|---|---|
| `src/include/op/scan/iceberg_delete_filter.hpp` | Replaced the inline `post_convert_fn_t` typedef with `#include <op/scan/post_convert_fn.hpp>` and a `using post_convert_fn_t = scan_post_decode_hook_t;` alias.  Alias dies in Phase 2 along with the legacy callers. |
| `src/include/op/scan/parquet_scan_operator_data.hpp` | Added `scan_post_decode_hook_t post_decode_hook;` field on `parquet_scan_data` (default null). |
| `src/op/scan/sirius_gpu_parquet_scan_operator.cpp` | After post-read filter eval, before `assemble_scan_output`, invoke `scan_data.post_decode_hook` with the first slice's `file_path` and a prefix-sum `first_row` (sum of `file_metadata->row_groups[i].num_rows` for `i < row_group_indices.front()`).  No-op when null. |
| `src/pipeline/sirius_pipeline_converter.cpp` | Added `insert_iceberg_scan_operator()` and a local `resolve_iceberg_table_path` helper that matches `sirius_engine::resolve_iceberg_table_path` (file-based: `parameters[0]`; REST catalog: derive from `MultiFileBindData` first file by stripping `/data/<filename>`).  Rewrote `split_table_scan_source()` to a three-way switch: parquet → parquet, seq_scan → duckdb_native (toggle removed), iceberg → new iceberg path. |
| `src/include/pipeline/sirius_pipeline_converter.hpp` | Declared `insert_iceberg_scan_operator`. |
| `CMakeLists.txt` | Added `iceberg_scan_info.cpp` and `iceberg_split_provider.cpp`. |

### Runtime behavior change in Phase 1

`iceberg_scan` queries no longer construct `sirius_physical_iceberg_scan` /
`iceberg_scan_task`.  They go through `sirius_gpu_parquet_scan` + the new
`iceberg_split_provider`.  The legacy `sirius_physical_iceberg_scan` /
`iceberg_scan_task` code still compiles but is unreachable for `iceberg_scan`
queries.  Same for `seq_scan` — no more toggle, always GPU-native.

### Verification needed before Phase 2

Run on a GPU-equipped machine (the test binary refuses to initialize without
GPUs):

```bash
pixi run make
build/release/extension/sirius/test/cpp/sirius_unittest --list-tests "*iceberg*"
build/release/extension/sirius/test/cpp/sirius_unittest "[iceberg]"
```

Key cases to validate (all live in
`test/cpp/integration/test_gpu_execution_multi_format.cpp`):

- Iceberg V1 (no deletes) — `count(*)` and `SELECT *` shapes
- V2 positional deletes — `order by desc` (the historically flaky case
  flagged with the `parquet_scan_task` byte-range mismatch TODO)
- V2 equality deletes — single-column, multi-column, and groups where the
  key column is NOT in the user projection (forces the eq-delete column
  widening path)
- V2 combined positional + equality
- V3 deletion vectors
- Schema evolution fixtures

Minimal manual smoke:

```sql
LOAD 'build/release/extension/sirius/sirius.duckdb_extension';
CALL gpu_execution('SELECT count(*) FROM read_parquet(''<some.parquet>'')');
CALL gpu_execution('SELECT count(*) FROM lineitem');   -- seq_scan, now unconditional GPU
CALL gpu_execution('SELECT fruit, count FROM iceberg_scan(''<fixture>'') ORDER BY count');
CALL gpu_execution('EXPLAIN SELECT * FROM lineitem WHERE l_orderkey < 100');
-- EXPLAIN should print GPU_PARQUET_SCAN / GPU_DUCKDB_NATIVE_SCAN — never the
-- legacy PARQUET_SCAN / DUCKDB_SCAN / ICEBERG_SCAN enumerators.
```

### Risks to watch during verification

1. **`first_row` for positional deletes** — `iceberg_split_provider` forces
   `max_file_processed = 1` so each `parquet_scan_data` covers a single file.
   `first_row` is computed by the operator as the prefix sum over
   `file_metadata->row_groups[i].num_rows` for row groups preceding the
   surviving range.  Validate against multi-row-group V2 positional fixtures.
2. **Hook timing vs `assemble_scan_output`** — chose to invoke the hook
   BEFORE assembly so eq-delete `_data_key_indices` resolve against the
   pre-assembly D-order; assembly then drops the trailing extras via
   `scan_output_arity`.  If assembly reorders columns past the key positions,
   we'd need to flip to after-assembly with indices computed in the
   post-assembly order.
3. **Iceberg cache key parity** — the converter now uses the same
   `resolve_iceberg_table_path` logic as `sirius_engine::prefetch_iceberg_delete_data`
   (file path or REST-derived).  The legacy converter used `parameters[0]`
   only, which silently missed for REST catalog paths.

---

## Phase 2 — TODO (delete legacy code)

Delete the files listed in the table below.  These two are the canonical
references — read both end-to-end before doing surgical edits, since their
member-by-member structure dictates which `task_creator` / `task_scheduler` /
`sirius_context` / `sirius_engine` references to unwire.

- [src/op/scan/parquet_scan_task.cpp](src/op/scan/parquet_scan_task.cpp)
- [src/op/scan/duckdb_scan_executor.cpp](src/op/scan/duckdb_scan_executor.cpp)

### Files to delete

| Path | Reason |
|---|---|
| `src/op/scan/parquet_scan_task.{hpp,cpp}` | Core legacy parquet task. |
| `src/op/scan/duckdb_scan_task.{hpp,cpp}` | Legacy CPU seq_scan task. |
| `src/op/scan/iceberg_scan_task.{hpp,cpp}` | Replaced by `iceberg_split_provider`. |
| `src/op/scan/duckdb_scan_executor.{hpp,cpp}` | Thread pool for the legacy task types. |
| `src/op/scan/parquet_schema_mapping.{hpp,cpp}` | Only used by `parquet_scan_task`. |
| `src/op/sirius_physical_parquet_scan.{hpp,cpp}` | Transient CPU wrapper operator. |
| `src/op/sirius_physical_iceberg_scan.{hpp,cpp}` | Transient CPU wrapper operator. |
| `src/op/sirius_physical_duckdb_scan.{hpp,cpp}` | Transient CPU wrapper operator. |
| `src/data/host_parquet_representation.{hpp,cpp}` | Only used by `parquet_scan_task`. |
| `src/data/host_parquet_representation_converters.{hpp,cpp}` | Same. |
| `src/include/data/cached_data_representation.hpp` | Grep before delete; only the doomed translation units include it. |
| `test/cpp/scan/test_parquet_scan_task.cpp` | Tests deleted code. |
| `test/cpp/scan/test_scan_executor.cpp` | Same. |
| `test/cpp/scan/test_parquet_schema_mapping.cpp` | Same. |
| `test/cpp/data/test_host_parquet_representation.cpp` | Same. |

**Kept iceberg helpers** (still consumed by the engine-time
`prefetch_iceberg_delete_data` flow):

- `src/op/scan/iceberg_metadata_reader.{hpp,cpp}`
- `src/op/scan/iceberg_avro_reader.{hpp,cpp}`
- `src/op/scan/puffin_reader.{hpp,cpp}`
- `src/op/scan/iceberg_delete_pipeline.{hpp,cpp}`
- `src/include/op/scan/iceberg_delete_filter.hpp` and its impls
  (`positional_delete_filter.cpp`, `equality_delete_filter.cpp`,
  `equality_delete_mask.cu`)

### Files to modify in Phase 2

| File | Surgical change |
|---|---|
| `CMakeLists.txt` | Remove deleted-file entries (~9 source + 4 test lines around L148-149, L193-209, L221, L234-235, L434, L521-526). |
| `src/creator/task_creator.{hpp,cpp}` | Delete `DUCKDB_SCAN`, `PARQUET_SCAN`, `ICEBERG_SCAN` branches of `prepare_for_query`, `get_operator_for_next_task`, `manager_loop`.  Remove `_parquet_scan_operator_global_state_map` + `_scan_operator_global_state_map` members and forward decls.  Keep `CPU_SOURCE`. |
| `src/pipeline/task_scheduler.{hpp,cpp}` | Remove `_scan_executor` member + ctor param, special-case `schedule()` branches for parquet/duckdb/iceberg tasks, `get_scan_executor()`. |
| `src/sirius_context.cpp` | Drop duckdb_scan_executor include + ctor wiring; drop `task_scheduler::cache_scan_results_for_query` call (legacy query-result caching goes away — flag in PR description). |
| `src/sirius_engine.cpp` | Delete the loop at ~L297-298 that wrote `iceberg_scan->delete_data` on the now-deleted CPU wrapper.  Keep `prefetch_iceberg_delete_data` + `iceberg_delete_data_cache_`.  The dead-code engine-side `construct_sirius_specific_operator` / `construct_iceberg_scan_operator` (L219-302) also go. |
| `src/pipeline/sirius_pipeline_converter.cpp` | Delete the TABLE_SCAN branches of `construct_sirius_specific_operator` (L62-90).  Drop its `iceberg_cache` parameter; update the four call sites (L392, L678, L695, L933 — only L392 currently used it).  Delete the `DUCKDB_SCAN` / `ICEBERG_SCAN` cases at L1114, L1293, L1338. |
| `src/pipeline/sirius_pipeline.cpp` | Delete the `DUCKDB_SCAN` / `ICEBERG_SCAN` branches of `update_pipeline_status` (L363-386) — `GPU_PARQUET_SCAN` / `GPU_DUCKDB_NATIVE_SCAN` are covered by the existing fallthrough. |
| `src/planner/query.cpp` | L47-48: drop `DUCKDB_SCAN` / `ICEBERG_SCAN` from the predicate; add `GPU_PARQUET_SCAN` / `GPU_DUCKDB_NATIVE_SCAN` if not already present. |
| `src/include/op/sirius_physical_operator_type.hpp` | Delete `DUCKDB_SCAN`, `PARQUET_SCAN`, `ICEBERG_SCAN` enumerators.  Keep `CPU_SOURCE`, `GPU_PARQUET_SCAN`, `GPU_DUCKDB_NATIVE_SCAN`.  (The separate `op::operator_data_type::PARQUET_SCAN` enumerator in `parquet_scan_operator_data.hpp` stays — that's the live data-type for the new path.) |
| `src/op/sirius_physical_operator_type.cpp` and `src/pipeline/sirius_plan_printer.cpp` | Delete the matching string-mapping cases. |
| `src/include/data/sirius_converter_registry.hpp` | Drop the `host_parquet_representation_converters.hpp` include. |
| `src/include/op/scan/iceberg_delete_filter.hpp` | Drop the `post_convert_fn_t` legacy alias (the only legacy caller has been removed). |

---

## Phase 3 — TODO (config + extension + docs cleanup)

- `src/sirius_config.{hpp,cpp}` — remove `enable_gpu_duckdb_native_scan`,
  `_scan_executor_config`, `get_duckdb_scan_executor_config`,
  `cache_scan_results_enabled`/`get_cache_level`/`set_cache_level` (if backed
  by `_scan_executor_config.cache`).  Drop YAML parser entries.
- `src/sirius_extension.cpp` — drop the `enable_gpu_duckdb_native_scan`
  extension parameter + default registration (~L1447, L1449, L1615, L1619).
- `src/sirius_context.cpp` L615 — remove the `enable_gpu_duckdb_native_scan`
  gate on `host_fsmr` provisioning; verify it's safe to allocate
  unconditionally.
- Doc/comment touch-ups: `docs/super-sirius/scan.md` (strip "Legacy:
  DuckDB-Managed Scan" section, L12-16, L193-230, L444, L472, L492-495),
  `docs/super-sirius/task-creator.md`, `docs/super-sirius/dynamic-filters.md`,
  `docs/super-sirius/pipeline-execution.md`,
  `docs/super-sirius/execution-flow.md`, `docs/glossary.md`.  Cosmetic comment
  cleanup at `src/op/sirius_physical_table_scan.cpp:113`,
  `src/include/op/scan/hive_partition.hpp:52-54`.

---

## Resuming on another machine

```bash
git fetch origin
git checkout kk/legacy-scan-cleanup-phase1
pixi shell
CMAKE_BUILD_PARALLEL_LEVEL=$(nproc) make

# Verify Phase 1 on a GPU:
build/release/extension/sirius/test/cpp/sirius_unittest "[iceberg]"

# When Phase 1 is verified, continue with Phase 2 deletions per the table
# above.  Recommended sequence inside Phase 2:
#   1. Delete the three legacy task types and the executor (+ their tests).
#   2. Delete the three transient CPU wrapper operators.
#   3. Rewire task_creator / task_scheduler / sirius_context / sirius_engine /
#      sirius_pipeline_converter.cpp to drop the legacy branches.
#   4. Delete host_parquet_representation* and the cached_data_representation
#      header (re-grep first).
#   5. Prune the SiriusPhysicalOperatorType enum + its string maps + plan
#      printer.
#   6. Build and run the full unit + integration suite.
```
