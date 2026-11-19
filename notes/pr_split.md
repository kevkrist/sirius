# PR split / attribution map: #2569 (S5B1) vs #2570 (S5B2)

Repo: `/opt/dlami/nvme/dfperf/repo`. Base = `c4fa3638` (merge-base origin/main). Line refs are at tip `pr2570`
unless marked `@2569`. Method: `git diff base..pr2569`, `git diff pr2569..pr2570`, `git blame pr2570`
(per-line owner), `git grep` at each tip. Nothing built or run.

## 1. One-paragraph answer

- **#2569 = mechanism, mostly dormant.** Adds all new GPU kernels + accumulators + policy
  (`construction_plan`, `build_filter_construction`, `exact_key_stager`, `bounds_accumulator`,
  `prepare_build_keys`, per-key-geometry Bloom accumulation) with unit tests. **Construction site is
  unchanged at pr2569**: single-partition/broadcast builds still use the one-shot
  `publish_dynamic_filters` at the join build-port deposit (after CONCAT); multi-partition builds still
  use the old INT32/INT64 Bloom-only accumulation at PARTITION.
- **#2570 = wiring + deletion.** Rewires `dynamic_filter_publication_session` onto
  `build_filter_construction`, makes every build PARTITION (hash *and* forward path) and BUILD_FILTER a
  construction site, deletes the one-shot path, removes `enable_dynamic_filter_multi_partition`, raises
  Bloom budget. Touches **zero GPU kernel lines** (only deletes one `try_create` overload in the .cu).

Commits:
- 2569: c4b4e135 (key outcome log line), cc827f71 (glossary), 3e510dd1 (certification extract),
  3b614fd2 (construction_plan, "Not wired"), 6409f4e0 (prepared keys), 4759770e (Bloom all key types),
  bef7d2f4 (stager + bounds, "Not wired"), e9c4eca3 (build_filter_construction), a2d6bd5a (style).
- 2570: e69f84ca (construct at every PARTITION), 25a98381 (right-family all key types), 5ccdcfcd (delete
  one-shot), f24221b7 (retire multi_partition knob), f7cdb605 (docs), 9a5069bf (Bloom budget), ae939824 (style).

## 2. Is 2569 code live at pr2569?

Evidence (`git grep` at pr2569, src only):
- `build_filter_construction`: only its own .cpp/.hpp + CMake -> **not wired** (tests only).
- `plan_construction`: only construction_plan.{cpp,hpp} -> **not wired**.
- `exact_key_stager`, `bounds_accumulator`: used only by build_filter_construction -> **not wired**.
- Session @2569 (`dynamic_filter_publisher.cpp@2569`): `try_begin_accumulation` :861 still gates on
  `plan.multi_partition_enabled()` and `accumulated_bloom_builder::supports()` (INT32/INT64);
  `observe_whole_build` :1162 and free `publish_dynamic_filters` :1357 (nvtx `dynfilter::push_build_side`)
  still exist and are called from `sirius_physical_hash_join::push_data_batch_partitioned`.

2569 code that **is live at pr2569** (affects perf of pr2569 alone):
| Piece | Where | Live via |
|---|---|---|
| `prepare_build_keys` / `prepare_build_column` | `src/cuda/dynamic_filter_prepared_keys.cu:131,166` | IN-list + small-IN-list column ctors (replaces `cudf::drop_nulls` + host sync) -> one-shot path |
| IN-list ctor from prepared rep, `construct()` | `sirius_dynamic_in_list_filter.cu:204-224`, small: `:149-187` | one-shot path |
| Bloom accumulation contribute rewrite (per-element convert, in-kernel string fingerprint, null skip) `add_accumulated_keys` | `sirius_dynamic_bloom_filter.cu:256-302` | old multi-partition path (INT32/INT64 only at 2569) |
| `accumulation_partial.cuh` (lease/stream/event plumbing moved out of bloom .cu) | whole file | old multi-partition path |
| certification extract `certify_decision`/`read_build_port_facts`, decline reasons | `build_input_certification.cpp:26,42` | observer + BUILD_FILTER |
| `[dynamic_filter_key]` line, `post_unlock_actions`, join identity | `dynamic_filter_key_outcome.cpp:91`, `sirius_pipeline_converter.cpp:397-421`, `sirius_plan_comparison_join.cpp:632-642,865-868` | all paths |
| `domain_gate.hpp::consider_bound_key`, `restore_scalar` | detail/ | one-shot + accumulated |

## 3. Per-file attribution (blame at pr2570: lines owned base / 2569 / 2570)

### src/cuda (kernels) — all 2569, 2570 = 0 lines
| File | base/69/70 | 2569 | 2570 |
|---|---|---|---|
| `accumulation_partial.cuh` | 0/222/0 | NEW: `accumulation_partial` :82 (per-GPU lease, exclusive stream, init/done events, `enqueue_contribution` :134 = StreamWaitEvent + EventRecord + StreamWaitEvent), `lease_partials` :168 (cudaMemsetAsync), `find_partial` :214 | - |
| `bounds_accumulator.cu` | 0/592/0 | NEW: `masked_bound` :97, `enqueue_bounds` :113 (CUB min/max), `try_create` :274, `contribution_scratch_bytes` :297, `enqueue_add` :334 (nvtx `dynfilter::bounds::contribute`; STRING via cuDF reduce under leaf mutex), `finish` :399 (nvtx `dynfilter::bounds::finish`), `key_bounds::to_scalars` :570 | - |
| `dynamic_filter_prepared_keys.cu` | 0/194/0 | NEW: `bounded_dense_write` :44, `bounded_compacting_write` :64, `compact_bounded` :99, `prepare_build_keys` :131, `prepare_build_column` :166 | - |
| `dynamic_filter_probe.cuh` | 424/79/0 | adds `require_prepared` :368, `current_device` :382, `convert_to_rep` :392, `materialize_string_fingerprints` :404, `string_fingerprint_at` :415, `with_string_fingerprint_iterator` :436, `with_build_key_iterator` :468 (build-side only; probe functors untouched) | - |
| `exact_key_stager.cu` | 0/362/0 | NEW: `try_create` :94, `contribution_scratch_bytes` :128, `enqueue_add` :144 (nvtx `dynfilter::stage::contribute`), `gather` :245 (nvtx `dynfilter::stage::gather`, cross-GPU copy to root), `gathered::build` :225 | - |
| `sirius_dynamic_bloom_filter.cu` | 1213/263/0 | `build_prepared_bloom_replica` :170-184, `add_accumulated_keys` :256-302 (rewritten), prepared-rep ctor :452-470, impl per-key shapes :719-747, `classify_keys` :987, per-key `try_create` :1005, `publication_scratch_bytes` :1030, `create` :1051, bits in `enqueue_add` :1100-1106 / `finish_impl` :1174-1181, `inspect_replica` :1428-1459. **Base-owned (pre-stack, not this stack's code)**: `chunk_pipeline` :772-968 (`dynfilter::accum::late_wait` :873), `or_bloom_chunk` :307, most of `finish_impl` :1130-1358, `replicate_to_devices` :547, `merge_from` :521, all probe (`compute_mask*`) | deletes shared-geometry `try_create(keys, geometry, targets)` (31 lines) only |
| `sirius_dynamic_in_list_filter.cu` | 404/92/0 | `capacity_factor_for`/`requested_capacity` :74-103 (shared math), ctor-from-prepared + `construct` :204-224, count-based `supports` :251-257, `set_slots` :439, `in_list_storage_upper_bound` :462 | - |
| `sirius_dynamic_small_in_list_filter.cu` | 305/44/0 | count-based `supports` :132-138, prepared ctor + `construct` :149-187 | - |

### src/op/dynamic_filter
| File | base/69/70 | 2569 | 2570 |
|---|---|---|---|
| `build_filter_construction.cpp` | 0/421/0 | NEW whole: `try_create` :104 (leases 3 accumulators, host arithmetic for slices), `reserve` :241, `contribute` :270 (bloom->stager->bounds on task stream, `join_on_failure`), `finish` :305 (bounds.finish, bloom.finish, stager.gather, per-key `build_within_room`, `root.stream.sync()` :398), `task_scratch_bytes` :402, `root_construction_bytes` :407 | - |
| `build_filter_construction.hpp` | 0/232/6 | NEW | `publication_root::space` -> const&, doc |
| `construction_plan.cpp/.hpp` | 0/226/20, 0/161/8 | NEW: `plan_construction` :107, `choose_membership` :80, `aligned_bloom_bytes` :57, Bloom cap (all-or-nothing) :204-225 | `bloom_for` :70, `construction_limits::{bloom_only,fixed_bloom_keys}`, `bloom_membership::rows`, drops `publication_path` arg |
| `complete_build_inventory.cpp/.hpp` | 126/176/11, 145/105/25 | `column_facts`, `facts()` :106, `read_arrival` :188 (decline reasons), ledger `record` :239 / `certify` :265 rewritten to return `certified_build_input`, `accumulated_bloom_chunk_bytes` :303 | accepts HOST/DISK arrivals via `metadata_facts` :167; ignores late empty batches (`holds_no_rows` :226); removes shared geometry (`accumulated_bloom_geometry`) |
| `build_input_certification.cpp/.hpp` | 0/70/0 | NEW `certify_decision` :26, `read_build_port_facts` :42 | doc only |
| `build_input_certifier.cpp/.hpp` | 0/0/84+112 | - | NEW: `on_arrival` :35, `decide` :42 (certify -> `session.begin`/`decline`), `contribute` :69. Shared by PARTITION observer and BUILD_FILTER |
| `dynamic_filter_publisher.cpp` | 691/271/311 | key-line plumbing (`attempt_counters` :80-160, `set_key_lines` :427, `record_key_outcomes` :440, `post_unlock_actions` use), decline handling in `try_begin_accumulation`, refactor of one-shot to use domain gate / prepared keys | **rewrites session**: `begin` :943 (plan_construction + `build_filter_construction::try_create`, nvtx `dynfilter::construction::begin`), `decline` :920, `contribute` :1095 (reserve under mutex, add outside), `publish_if_final` :1172, `state::publish` :627 (finish -> `replicate_to_devices` loop :650 -> cross-rank merge -> `fan_out`), `task_scratch_bound` :1079, `terminal` :1089, `end_if_channels_closed` :527, `settle` :561. **Deletes** `try_begin_accumulation`, `observe_whole_build`, free `publish_dynamic_filters` (~360 lines, one-shot builder), `accumulation_claimed`. File 2569: 1763 lines -> 2570: ~1290 |
| `dynamic_filter_publisher.hpp` | 140/12/66 | `set_join_identity`, `certified_build_input` param | new session API (`begin/decline/contribute/publish_if_final/task_scratch_bound/terminal`) |
| `dynamic_filter_stats.hpp` | 166/14/97 | +7 counters (4 `accumulations_declined_*`, `keys_sized_on_total_rows`, `filters_skipped_construction_{invariant,admission}`) | doc per counter; retires `publications_skipped_source_not_resident`, `publications_skipped_build_not_whole`, `keys_skipped_bloom_unsupported` |
| `hash_join_partition_observer.cpp/.hpp` | 53/0/19 | (2569 edit to use `certify_decision`, later replaced) | delegates to `build_input_certifier`; `on_hash_input` :39 (MARK facts split out), `before_transform` :45, `after_transform` :53 -> `publish_if_final`, `task_scratch_bytes` :67 |
| `sirius_physical_build_filter.cpp/.hpp` | 87/0/16 | (same pattern: 2569 edit replaced) | uses certifier; decline => Terminal; `no_history_peak_memory_estimate` :92 adds scratch |
| `detail/accumulated_bloom_builder.hpp` | 316/59/7 | per-key `key::blocks`, `create`, `publication_scratch_bytes`, widened key types | removes `supports()` (INT32/64 gate) and shared-geometry `try_create` |
| `detail/exact_key_stager.hpp`, `detail/bounds_accumulator.hpp`, `detail/restore_scalar.*`, `prepared_build_keys.hpp`, `dynamic_filter_key_outcome.*`, `post_unlock_actions.*` | 0/all/0 | NEW | - |
| `detail/domain_gate.hpp` | 0/66/6 | NEW `consider_bound_key` | drops `publication_path` enum (log now always `path=construction`) |
| `sirius_dynamic_filter.hpp` | 734/93/0 | prepared-rep ctors, count-based `supports`, `in_list_storage_upper_bound`, `make_accumulated` decl | - |
| `dynamic_filter_publish_plan.hpp`, `cross_rank_filter_merge.hpp`, `bloom_sizing.hpp`, `config.hpp` | base/0/3 each | - | removes `enable_multi_partition` / `multi_partition_enabled()`; doc |

### operators / planner / pipeline / config
| File | 2569 | 2570 |
|---|---|---|
| `src/op/sirius_physical_partition.cpp` | - | forward path (non-hash or count<2) now calls `before_transform` :160-163 and `after_transform` :193-195; hash path adds `on_hash_input` :230; `no_history_peak_memory_estimate` :506-518 adds `task_scratch_bytes` (and no longer returns 0 for forward path when scratch != 0) |
| `src/op/partition/partition_input_observer.hpp` | - | adds `on_hash_input`, `task_scratch_bytes` |
| `src/op/partition/consumer_partition_policy.cpp` | - | drops `build_arrives_whole` computation (`set_concat_all` logic kept) |
| `src/op/sirius_physical_hash_join.cpp` | - | deletes `push_data_batch_partitioned` override (one-shot claim at build deposit) and `set_build_arrives_whole` |
| `src/op/sirius_physical_hash_join.hpp` | join-identity plumbing (5 lines) | removes whole-build flags/override |
| `src/pipeline/sirius_pipeline_converter.{cpp,hpp}` | `set_join_identity` per join after operator numbering :397-421 (logging only) | - |
| `src/planner/sirius_plan_comparison_join.cpp` | join signature for key lines :632-642, :865-868 | removes right-family INT32/64 key filter and `needs_early_publication` gate; drops `.enable_multi_partition` |
| `src/planner/dynamic_filter/build_filter_evidence.cpp` | - | `right_family_publication_enabled` = `enable_dynamic_filter` only (:99) |
| `src/planner/sirius_physical_plan_generator.cpp` | - | BUILD_FILTER inserted whenever join publishes (:1640) |
| `src/sirius_config.{hpp,cpp}`, `src/sirius_extension.cpp` | - | removes `enable_dynamic_filter_multi_partition` (YAML throws, SET unknown); `max_dynamic_filter_bloom_bytes_per_gpu()` = max(4 x scan_task_batch_size, max_build_hash_table_bytes) (`sirius_config.hpp:272`) |

## 4. Rules: where does an optimization belong?

1. **Kernel / accumulator internals -> #2569.** Anything in `src/cuda/{accumulation_partial.cuh,
   bounds_accumulator.cu, exact_key_stager.cu, dynamic_filter_prepared_keys.cu}`, the build-side helpers in
   `dynamic_filter_probe.cuh:364-503`, IN-list/small-IN-list construct paths, `add_accumulated_keys` and
   per-key Bloom shapes, `build_filter_construction.cpp` (`try_create/reserve/contribute/finish`),
   `construction_plan.cpp` policy (filter-kind choice, sizing, cap logic), `detail/*.hpp`.
   Caveat: at pr2569 only prepared-keys/IN-list ctor + Bloom contribute are exercised by live queries; the
   rest is verifiable at pr2569 only by unit tests/microbenchmarks. TPC-H effect measurable only at pr2570.
2. **Session orchestration / call sites / scheduling -> #2570.** `dynamic_filter_publisher.cpp`
   (`begin`, `contribute`, `publish_if_final`, `state::publish`, locking, replicate loop, fan-out order),
   `build_input_certifier.*`, `hash_join_partition_observer.*`, `sirius_physical_build_filter.*`,
   `sirius_physical_partition.cpp` hooks + memory estimate, `partition_input_observer.hpp`, HOST/DISK
   arrival acceptance + late-empty handling in `complete_build_inventory`, planner right-family changes,
   config (Bloom budget formula, knob removal), `bloom_for`/`fixed_bloom_keys`/`bloom_only`.
3. **Touching both -> split.** API/kernel change (new accumulator method, signature, struct field,
   kernel fusion) + its unit test in #2569; the session/observer call-site change that uses it in #2570.
   Because 2569 code is dormant, a 2569 API change must still compile and pass 2569's unit tests with the
   OLD session (`try_begin_accumulation`, shared-geometry `try_create`, `supports()` still exist there).
   Do not make a 2569 change depend on anything 2570 adds (`build_input_certifier`, `session.begin`).
4. **Deleted-in-2570 code** (one-shot `publish_dynamic_filters`, `observe_whole_build`, shared geometry):
   never optimize; dead at tip.
5. **Base-owned code** (Bloom `chunk_pipeline`/`finish_impl` reduce+replicate, `replicate_to_devices`,
   probe/`compute_mask`, scan-side `dynamic_filter_merge.cpp`): pre-stack. It is now on the path of every
   build (2570 made single-partition builds use it), so a regression there is *exposed* by 2570; fixes
   are arguably out of the stack, or go in #2570 if the change is about how the session drives it. The
   HOST-staged Bloom transport lives here — explicitly out of scope per user.
6. Tests follow code: kernel/accumulator tests (section 7, "69" column) in #2569; session/operator/
   integration tests in #2570.

## 5. Config knobs (tip)

| Knob | Default | Declared / read | Note |
|---|---|---|---|
| `enable_dynamic_filter` | true | `sirius_config.hpp:264`, YAML `sirius_config.cpp:674`, SET `sirius_extension.cpp:3636` | sole on/off; A/B switch |
| `enable_dynamic_zone_map_filter` | **false** | `sirius_config.hpp:290`, `.cpp:685`, SET :3644 -> `emit_zone_map_filters` (`sirius_plan_comparison_join.cpp:~849`) | zone maps + bounds accumulator off by default (bounds still needed for DECIMAL128 fit) |
| `dynamic_filter_domain_coverage_threshold` | 0.9 | `config.hpp:26`, `sirius_config.hpp:296`, SET :3938 | domain gate (`detail/domain_gate.hpp`) |
| `dynamic_filter_inlist_max_l2_fraction` | 0.125 | `sirius_config.hpp:301`, `.cpp:689` (YAML only) | IN-list vs Bloom choice vs device L2 (`device_l2_cache_bytes` publisher.cpp:212) |
| `dynamic_filter_keep_threshold` | 0.9 | `sirius_config.hpp:305`, SET :3955 | consumer-side (scan), not construction |
| Bloom budget (derived) | max(4 x `scan_task_batch_size`, `max_build_hash_table_bytes`) | `sirius_config.hpp:272` (2570; 2569 = 4 x scan batch) | not settable. With specific_config: max(16 GB, 8 GB) = 16 GB/GPU, same at both tips |
| `enable_dynamic_filter_multi_partition` | removed in 2570 (true @2569) | YAML throws `sirius_config.cpp:675` | specific_config does not set it -> OK |
| `max_dynamic_filter_bloom_bytes_per_gpu` (YAML) | removed pre-stack | throws :680 | |
| fallback `default_max_dynamic_filter_bloom_bytes_per_gpu` | 3.2 GB | `op/dynamic_filter/config.hpp:35` | standalone policies/tests only |
| policy-only: `bloom_only`, `fixed_bloom_keys`, `publish_when_empty_or_drained` | off | `dynamic_filter_publish_plan.hpp:63+` | distributed cross-rank merge only |

specific_config.yaml DF-relevant: `scan_task_batch_size: 4GB`, `hash_partition_bytes: 1GB`,
`concat_batch_bytes: 1GB`, `max_build_hash_table_bytes: 8GB`, `max_broadcast_join_size: 3GB`, 4 GPUs,
`enable_pin_table_compression: true`, quent on. No DF keys set -> defaults (DF on, zone maps off).
**Watch**: compression is on, and a compressed arrival at a construction site declines the whole
attempt (`read_arrival` `complete_build_inventory.cpp:193` -> `COMPRESSED_ARRIVAL`); ORDER BY producers
decline too (`PIPELINE_BARRIER`). Check `accumulations_declined_compressed_arrival` first — a decline is
lost filtering vs the old one-shot path (which built from the decoded CONCAT output).

## 6. Diagnostics

### Counters — `dynamic_filter_stats.hpp:133-179`
Read via `SiriusContext::get_dynamic_filter_stats_snapshot()` (`sirius_context.hpp:905`); cumulative; no SQL
surface found (tests use `sirius::test::get_dynamic_filter_stats_snapshot(con)`).
- Attempt lifecycle: `accumulations_started`, `accumulation_expected_contributions`,
  `accumulation_completed_contributions`, `accumulation_duplicate_contributions`,
  `accumulations_incomplete`, `accumulations_abandoned`, `accumulation_storage_leaks`.
- Declines/skips: `accumulations_skipped_inventory`, `accumulations_declined_{pipeline_barrier,
  port_directive,unreadable_arrival,compressed_arrival}`, `accumulations_skipped_{admission,error,transient}`.
- Publication: `accumulation_publications_finished`, `accumulation_publications_host_staged`,
  **`accumulation_publication_latency_ns`** (final contribution -> first visible), `publication_attempts`,
  `publications_finished`, `publications_failed`, `publications_skipped_targets_drained`, `filters_pushed`.
- Keys/filters: `keys_considered`, `keys_with_known_domain`, `keys_skipped_domain_gate`,
  `keys_skipped_type_mismatch`, `keys_build_exceeded_domain`, `keys_skipped_bloom_size_gate`,
  `keys_sized_on_total_rows`, `membership_filters_built`, `zone_map_filters_built`,
  `filters_skipped_construction_{invariant,admission}`, `bindings_skipped_incompatible_probe`,
  `producers_enabled`.
- Consumer: `applications_skipped_other_device`, `cached_*`.

### Log lines
- `[dynamic_filter_key] v=1 sig=.. occ=.. join=<op id> condition= build_ordinal= rows= valid= kind= size=
  zone_map= pushed= reason=` — DEBUG, one per admitted key with a binding at session terminal
  (`dynamic_filter_key_outcome.cpp:91`). Best per-join/per-key record (2569).
- `[dynamic_filter_publication_session] construction started: K key(s), B batch(es) on G GPU(s).` INFO
  (publisher.cpp:1055, 2570).
- `[dynamic_filter_publication_session] constructed filters published[ (Bloom through HOST staging)]: N
  filter(s) pushed to T target(s), X ms after the final contribution.` INFO (:717, 2570) — publish latency.
- `[build_input_certifier] <op> (id=) constructs no dynamic filter: <decline>.` INFO
  (build_input_certifier.cpp:54, 2570).
- `[dynamic_filter_publication_session] construction skipped: an accumulator lease was refused.` DEBUG (:1021).
- `[dynamic_filter_publication_session] construction declined: a task input is not exactly ...` (:833).
- `[dynamic_filter_publication_session] accumulation settled: outcome=` DEBUG (:585).
- `[dynamic_filter_domain] path=construction condition= rows= domain= provenance= eligible= threshold=
  decision=` (`detail/domain_gate.hpp:59`).
- `[accumulated_bloom_builder::finish] root GPU {} reduced and replicated {} key(s), {} bytes in ...`
  (`sirius_dynamic_bloom_filter.cu:1346`, base) — Bloom reduce/replicate timing.
- `[dynamic_filter_consumer] a batch on device {} passed through unfiltered ...` (scan side).

### NVTX ranges (for nsys)
`dynfilter::construction::begin` (publisher.cpp:946), `dynfilter::accum::contribute` (bloom .cu:1084),
`dynfilter::stage::contribute` (stager :150), `dynfilter::bounds::contribute` (bounds :340),
`dynfilter::accum::finish` (:1137), `dynfilter::accum::late_wait` (:873), `dynfilter::stage::gather` (:248),
`dynfilter::bounds::finish` (:402), `dynfilter::replicate_devices` (publisher.cpp:650), marks
`dynfilter::accum::final_commit` (:1159) / `dynfilter::accum::visible` (:713); consumer:
`dynfilter::try_fused_membership_mask`, `dynfilter::apply_output`. Gone at tip: `dynfilter::push_build_side`
(one-shot; useful as the baseline range on base/pr2569).

## 7. Tests (test/cpp), by owning PR (added/removed lines 69 | 70)

Kernel/accumulator/policy (2569):
- `operator/test_dynamic_filter_prepared_keys.cpp` `[dynamic_filter][prepared_keys]` (324 | -)
- `operator/test_accumulated_bloom_key_types.cpp` `[dynamic_filter][construction][bloom]` (248 | -)
- `operator/test_exact_key_and_bounds_accumulation.cpp` `[..][construction][stager]`, `[..][bounds]` (510 | -)
- `operator/test_build_filter_construction.cpp` `[dynamic_filter][construction]`, `[tracked]`, `[in_list_bound]` (738 | 41/43)
- `operator/test_dynamic_filter_construction_plan.cpp` `[dynamic_filter][construction_plan]` (479 | 24/6)
- `operator/test_build_input_certification.cpp` `[dynamic_filter][certification]` (121 | -)
- `operator/test_complete_build_inventory.cpp` `[..][inventory]`, `[bloom_budget]` (256 | 179/113)
- `operator/test_dynamic_filter_key_outcome.cpp` `[dynamic_filter][key_outcome]` (371 | 47/64)
- helpers: `construction_parity_utils.hpp` (386 | 51/48), `dynamic_filter_column_test_utils.hpp` (208 | -)

Session/operator/integration (2570):
- `operator/test_dynamic_filter_accumulation.cpp` `[dynamic_filter][construction]`, `[mgpu][multi_gpu]` (188/106)
- `operator/test_dynamic_filter_publisher.cpp` `[dynamic_filter][publisher]`, `[cross_rank]` (259/39)
- `operator/test_dynamic_filter_publication_claim.cpp` `[publication_claim]` (49/367, one-shot tests removed)
- `operator/test_physical_partition.cpp` `[physical_partition][dynamic_filter][construction]` (259/35)
- `operator/test_physical_build_filter.cpp` `[dynamic_filter][build_filter]`, `[scheduling]`, `[memory_estimate]` (145/11)
- `pipeline/test_in_task_publication.cpp` `[pipeline][dynamic_filter][in_task_publication]`, `[mgpu]`, `[memory]` (183/6)
- `integration/test_gpu_execution_dynamic_filter_multi_partition.cpp` `[integration][gpu_execution][dynamic_filter][construction]` (28/38)
- `integration/test_gpu_execution_dynamic_filter_sip.cpp` `[right_family]`, `[delim_cte_proof]`, `[null_safe_rf]` (71/36)
- `config/test_config.cpp` `[config_opt][dynamic_filter]`, `config/test_context*.cpp`, `exchange/test_exchange_collectives.cpp`,
  `planner/test_build_filter_evidence.cpp`, `planner/test_plan_tree_shape.cpp`,
  `operator/test_dynamic_filter_accumulation_fault.cpp` (`[.][cuda_status_fault]`),
  `operator/test_physical_hash_join_mgpu.cpp`, `utils/dynamic_filter_publication_fixture.hpp`
- deleted: `operator/test_dynamic_filter_readiness_fault.cpp`
- host-alloc fault: `scripts/run_dynamic_filter_host_allocation_fault.py` (`[.][host_allocation_fault]`)

Run e.g. `pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter][construction]"`.

## 8. Perf-relevant structure noticed while mapping (leads, unmeasured)

- Per contribution (`build_filter_construction::contribute` :270): up to 3 separate passes over each key
  column on the task stream (Bloom add, exact stage, bounds reduce), each wrapped by
  `accumulation_partial::enqueue_contribution` (2 StreamWaitEvent + 1 EventRecord). Fusion candidate -> #2569.
- `finish` :305 ends with a blocking `root.stream.sync()` :398 on the PARTITION task that completes the
  inventory, then `replicate_to_devices` per exact filter/zone map (publisher.cpp:650) — all inside the
  PARTITION task, before its `after_transform` returns (2570 scheduling; kernels 2569/base).
- `contribute` (publisher.cpp:1095) takes the session mutex twice per batch plus host
  `batch_key_facts::of` under the mutex -> #2570.
- Forward-path PARTITION (single partition/broadcast) previously reserved 0 bytes; now reserves
  2 x input + scratch when scratch != 0 (`sirius_physical_partition.cpp:509-518`) — may lower task
  concurrency under memory pressure -> #2570.
- Single-partition builds lost the one-shot path's single-batch build (after CONCAT) and now go through
  per-batch accumulate + root reduce + replicate (base Bloom `finish_impl`, possibly HOST-staged).
