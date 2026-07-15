# PR E0 — mixed-chunk pin support (concrete design)

- Status: **implemented and statically re-audited**. Post-audit contract corrections are present;
  build/runtime verification remains pending. This is the E0 storage/registry unit of PR E and
  remains independent of Simpatico PRs B–D.
- Replaces the earlier PR E0 sketch, which assumed a `pinned_chunk_ref` + `logical_chunks` model
  that does not exist. The companion [implementation plan](zero-copy-implementation-plan.md) now
  summarizes this concrete `cached_databatch_provider` / `pinned_entry` design.

## 0. The problem, precisely

A compression-enabled pin materializes chunks one at a time; three independent per-chunk
conditions route a chunk to the **uncompressed** arm even when compression is on:

- sub-threshold: `uncompressed_bytes < min_batch_size_bytes`;
- ratio reject: compressed footprint too large — logs, but does **not** latch;
- exception latch: the first failing chunk sets `compression_failed`; chunks
  already compressed stay compressed, later chunks go raw.

So a batch commonly ends with **both** `compressed_chunks` and the raw arm
(`host_chunks` / `tables`) non-empty — a *mixed set*. PR A's temporary install guards threw on
that state to avoid dropping rows; E0 removes those guards only after giving mixed sets an explicit
serving model.

If the guards were simply deleted with no further change, the install site's
`!compressed.empty() && raw.empty()` test flips false, the `else` branch pins **only the raw arm**,
and the compressed chunks are silently dropped — missing rows. So E0 is a real feature, not a
guard removal.

## 1. Pre-E0 model and failure mode

Before E0, `pinned_entry` had separate raw and compressed storage members but no interleaving
order. `cached_databatch_provider` derived its chunk count from one arm per tier and always chose
compressed storage when present, so it could not serve a mixed result.

The raw GPU path pairs each dense per-column chunk slot with the placement at the same index.
Null-padding the raw arm to logical positions would break that alignment because raw columns and
placements are stored densely. E0 therefore keeps both arms dense and adds an explicit logical
index rather than changing the raw representation.

## 2. Core design — a logical-order index over two dense arms

Add one small type and one field. Both arms stay **dense** (each holds only its own chunks); a
per-logical-chunk descriptor says which arm and which dense slot serves each scan position.

```cpp
// pinned_chunk_source.hpp
enum class chunk_kind : std::uint8_t { raw, compressed };

/// One logical chunk's location within a MIXED entry: which arm holds it and its
/// dense index in that arm.
struct chunk_source {
  chunk_kind    kind;
  std::size_t   arm_index;
};
```

```cpp
// added to struct pinned_entry
/// Interleaving order for a MIXED entry, one entry per logical chunk in scan order.
/// EMPTY for a homogeneous entry (all-raw or all-compressed) — those keep the
/// single-arm fast path unchanged. When non-empty: arm_index indexes
/// compressed_{host,device}_chunks for `compressed`, and the raw arm
/// (host_chunks, or data_batches_by_column + chunk_memory_spaces) for `raw`.
std::vector<chunk_source> logical_order;
```

**Invariant.** `logical_order` empty ⇒ homogeneous, behaves exactly as today (zero regression for
every existing pin). `logical_order` non-empty ⇒ `size() == total logical chunks`; each `arm_index`
is a valid dense index into its arm; each referenced chunk is non-null. Both arms remain dense, so
the raw serve's `col_chunks.at(arm_index)` and `chunk_memory_spaces.at(arm_index)` stay aligned —
**the ledger's "logical-order skew" dissolves by construction rather than being patched.**

## 3. Provider change (`cached_databatch_provider`)

The provider snapshots both dense arms and `logical_order` while the registry entry is stable. When
`logical_order` is non-empty, `_n_chunks = logical_order.size()`; otherwise the homogeneous
per-tier selection is unchanged.

Factor the two existing arm-serving blocks into helpers and dispatch by `logical_order` when present:

```cpp
std::shared_ptr<cucascade::data_batch> get_device_databatch(std::size_t index) {
  if (!_logical_order.empty()) {
    auto const src = _logical_order.at(index);
    return src.kind == chunk_kind::compressed ? serve_compressed_device(src.arm_index)
                                              : serve_raw_device(src.arm_index);
  }
  // unchanged compressed-else-raw homogeneous fast path
}
```

- `serve_compressed_device(i)` projects `compressed_device_chunks[i]`.
- `serve_raw_device(i)` uses `i` for both `col_chunks.at(i)` and
  `chunk_memory_spaces.at(i)`.
- Host tier mirrors this with `serve_compressed_host` / `serve_raw_host`.

No consumer change: the scan operator already accepts both batch shapes (compressed batches are
decompressed on demand in `scan_operator_input::prepare_for_processing`; raw batches are
`gpu_table_representation`). Interleaving only selects which shape per index.

## 4. Materializer + result struct change (record the order)

The materializers push `base_row_count_per_chunk` for **every** chunk in emission order and append a
parallel `chunk_source` marker after storing that chunk in its dense arm. Each result struct carries:

```cpp
std::vector<sirius::scan_manager::chunk_source> logical_order;  // host_pin_result & device_pin_result
```

The arm index is a `std::size_t`, matching its vector:

```cpp
out.logical_order.push_back({chunk_kind::compressed, out.compressed_chunks.size() - 1});
out.logical_order.push_back({chunk_kind::raw, raw_arm.size() - 1});
```

`logical_order.size()` then equals the chunk count, and its order is parallel to
`base_row_count_per_chunk`. Publication validates those MVCC counts elementwise against the
logical storage order.

## 5. Generalized insert (one per tier, no `*_mixed` twins)

The two compressed inserts consume named value bundles, so raw tables, placements, and ordering
cannot be swapped accidentally among adjacent same-typed arguments:

```cpp
struct host_pinned_chunks {
  std::vector<std::shared_ptr<compressed_host_representation>> compressed;
  std::vector<std::shared_ptr<host_data_representation>> raw;
  std::vector<chunk_source> logical_order;
};

struct device_pinned_chunks {
  std::vector<std::shared_ptr<compressed_device_representation>> compressed;
  std::vector<std::unique_ptr<cudf::table>> raw;
  std::vector<memory_space*> raw_memory_spaces;
  std::vector<chunk_source> logical_order;
};

void insert_pinned_entry_host_compressed(
  const std::string& name, cache_entry_info cache_info,
  host_pinned_chunks chunks, memory_space& memory_space,
  std::unique_ptr<duckdb_mvcc_metadata> mvcc = nullptr);

void insert_pinned_entry_device_compressed(
  const std::string& name, cache_entry_info cache_info,
  device_pinned_chunks chunks, memory_space& memory_space,
  std::unique_ptr<duckdb_mvcc_metadata> mvcc = nullptr);
```

Both inserts validate the complete bundle before publication and always **replace** (compressed and
mixed entries do not participate in the raw per-column merge; see §7). The device raw-table
transpose (`table->release()` → `data_batches_by_column`) is shared through
`release_tables_into`; `num_rows` is a checked sum of both arms.

## 6. Install-site change + guard deletion

The mixed-set guards are removed. If any chunk compressed, the install site passes both arms and
their `logical_order` through the named bundle; if nothing compressed, it keeps the pure-raw insert
and its homogeneous merge semantics. DuckDB MVCC ownership is constructed before either call and
moves with the storage:

```cpp
auto mvcc = make_pin_mvcc(std::move(host_result.base_row_count_per_chunk));
if (!host_result.compressed_chunks.empty()) {
  host_pinned_chunks chunks{
    .compressed = std::move(host_result.compressed_chunks),
    .raw = std::move(host_result.host_chunks),
    .logical_order = std::move(host_result.logical_order)};
  scan_mgr.insert_pinned_entry_host_compressed(
    name, std::move(cache_info), std::move(chunks), *representative_host_space, std::move(mvcc));
} else {
  scan_mgr.insert_pinned_entry_host(
    name, std::move(cache_info), std::move(host_result.host_chunks),
    *representative_host_space, std::move(mvcc));
}
```

Device tier mirrors this compressed/generalized-vs-raw choice and the same atomic MVCC transfer.

## 7. Validation and carve-outs

- **`validate_pinned_entry_for_serving`** validates tier/storage exclusivity, schemas, non-null
  owners and placements, dense raw arms, per-column row alignment, checked totals, and the
  one-to-one `logical_order` permutation before publication or serving. Homogeneous zero-chunk
  entries remain valid.
- **MVCC is atomic with storage.** DuckDB identities require preallocated metadata and parquet or
  identity-free entries reject it. Counts must match the logical chunk rows elementwise and in
  total. Storage plus metadata publish under one registry lock, so no serviceable DuckDB entry is
  visible without its snapshot fence.
- **Merge + mixed is out of scope.** Only an existing homogeneous raw GPU entry can take the
  per-column merge path. It must have the same cache source, total rows, placements, and chunk row
  boundaries. The merged value is built and validated off-map, then committed by no-throw swap;
  compressed and mixed entries always replace. Refreshed MVCC moves in the same commit.
- **Snapshot boundaries are explicit.** Providers retain shared chunk ownership across re-pin or
  removal. `visit_pinned_entries` snapshots chunk owners and MVCC under the mutex, then runs
  callbacks after unlocking so visitor re-entry cannot deadlock.
- **Unchanged:** the decode/consumer shape and every homogeneous pin's empty-`logical_order`
  dispatch fast path.

## 8. Tests

Implemented coverage (not executed during this audit, per review instruction):

- A true SQL integration case writes a small and a large Parquet file, pins them through the real
  host and GPU materializers with a threshold between their sizes, asserts one raw plus one
  compressed arm and a two-element `logical_order`, then scans through the cache/decompression
  path and checks the aggregate over all 10,010 rows.
- Synthetic provider cases cover host/device interleaving, projection, ownership snapshots across
  re-pin/removal, homogeneous fast paths, and raw-GPU placement alignment.
- Validation cases reject malformed schemas, tiers, null storage, projected stored chunks, and
  non-dense/duplicate/out-of-range logical-order references before publication or serving.
- Re-pin cases establish that only a homogeneous equal-row raw-GPU entry uses the column-merge
  path; compressed and mixed entries are replaced.
- Atomic-publication cases reject DuckDB storage without MVCC, refresh metadata on a staged raw
  merge, reject same-total/different-boundary metadata, and re-enter registry mutation from a
  visitor callback after the mutex has been released.
- The in-memory reader reconstructs a selected nullable string/integer subset and verifies that
  only the selected leaf buffers are fetched.

Still required before merge: build/runtime execution of those tests, external-payload transaction
failure injection, and the resource-gated >2 GiB projection-decline case.

## 9. File-by-file change list + budget

| file | change | ~net lines |
|---|---|---|
| `pinned_chunk_source.hpp` | `chunk_kind` + `chunk_source` | +22 |
| `sirius_scan_manager.hpp` | `logical_order`, named chunk bundles, generalized insert/MVCC contracts | original estimate included above |
| `sirius_scan_manager.cpp` | provider `_n_chunks` + dispatch + 4 serve helpers (refactor of existing blocks); generalized insert bodies + shared `release_tables_into`; validation branch | +60 |
| `pin_table.hpp` | `logical_order` field on both result structs | +4 |
| `pin_table.cpp` | record `chunk_source` at each push (4 sites) | +8 |
| `sirius_extension.cpp` | delete 2 guards; rewrite 2 install branches | +10 / −8 |
| test | mixed-chunk behavioral test, both tiers | +~110 (separate budget) |

The estimates above describe the original E0 design, not the completed whole-PR footprint. Against
the PR D `HEAD` baseline, current PR E tracked product files are **+2,183/−731**, plus the 22-line
new `pinned_chunk_source.hpp`: **net +1,474 product lines**. Tests and design documentation are
excluded. This is +1,224 above the net +250 budget (about 490% over, or 5.90× the budget), well
beyond the plan's +30% review signal.

The excess is not only prose: E0 adds validation/snapshot/re-pin hardening; E1 adds the external
payload transaction; and serving adds selective reconstruction, converter bounds/device
contracts, and reservation accounting. Review and land it as separate E0 (mixed
storage/provider), E1 (staged pin transaction), and E2 (selective serving and sizing hardening)
commits—or grant an explicit whole-PR budget exception. It must not be treated as satisfying the
original +250 estimate.

## 10. Sequencing

E0 is architecturally independent of PRs B–D and of the staged-compress API, although both E0 and
E1 are now present in this worktree. In a reviewable commit series, land E0 before E1: the staged pin
flip makes mixed outcomes routine, and without E0 the guard would fire (or, if removed, data would
drop). Land the selective serving/sizing hardening after the storage and transaction contracts it
protects.
