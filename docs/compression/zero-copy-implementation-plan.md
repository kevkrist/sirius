# Zero-copy string compression — implementation plan (v10-frozen, minimal-footprint)

- Status: PR A + PR B landed. PR C is implemented in this working tree and is pending review.
  The audited PR D implementation is present and locally verified; PR E and PR F remain future
  work. Before submission, C and D still need isolated footprint measurements and a budget decision.
- Companion to: [zero-copy-string-compression.md](zero-copy-string-compression.md) (the design).
  This document sequences that design. It does not revive attempt #1 (`stash@{0}`, evaluated
  2026-07-09); stash references are evidence only.
- Revision note (2026-07-14): PR D no longer promises accept-time transfer out of an intact cuDF
  root. The accepted `compressed_table` is self-contained before inspection. Root projections
  remove intermediate copies only when a downstream codec materializes its own output.

## 1. Constraints and binding decisions

| area | decision | reason |
|---|---|---|
| Wire format | Keep v10 byte-identical: no version bump, meta slot, leaf tag, payload alignment, or emitted-buffer-set change. | `.hpln` persistence is out of scope, but in-process readers and writers must remain compatible. |
| Owned compression | Keep each original root structurally intact until the decision. Lend root views only to downstream materializing codecs; every candidate leaf owns its bytes before `table()` returns. | Reject can return the exact allocations. Accept needs only completion, moves, and destruction. |
| No accept-time detach | Do not use terminal placeholders, commit bindings, `cudf::column::release()`, or delayed reconstruction. | `column::release()` is declared `noexcept` but allocates wrapper objects internally; host OOM can terminate. The pinned API has no allocation-free detach primitive suitable for a no-fail commit. |
| Executor | PR D is single-stream. Every prior async read or write of the input must happen-before the stage stream, and no other stream may continue using transferred storage. | A stream passed to the API is an ordering contract, not an automatic producer fence. |
| Inspection | Expose a read-only `compressed_table_inspection`, bound to the stage stream, instead of `compressed_table const&`. | Public `compressed_table::columns` and shallow `unique_ptr` constness would otherwise permit mutation and invalidate transaction invariants. |
| Device and resources | Capture the current CUDA device; re-enter it for decisions and destruction. Streams and memory resources are borrowed and have explicit lifetimes. | RMM buffers retain allocator/deallocation metadata, and the process current device may change before a decision. |
| Pool executor | Defer until its first caller. It needs a real producer→worker fence and stream/resource lifetime design. | A local pool may die before accepted RMM buffers whose deallocation metadata names its streams. |
| STRING identity serialization | Reject raw STRING leaves and route production STRING plans through str_split. | Splitting the v10 emitted buffer set is not allowed. |
| Destructive decode | Select it explicitly with `decompress_column(PlanTree&, consume_tag, ...)`; base reps copy, only identity and str_split consume. | Dereferencing a const `unique_ptr<PlanTree>` still produces a mutable tree, so an untagged overload is unsafe. |
| Device-payload borrowing | Keep it in PR F, off by default and sanitizer/profile gated. | It removes only compressed-byte-sized D2D and introduces separate alignment and cross-stream lifetime concerns. |

The result is conditional copy elimination, not a claim that every owning-compression path is
zero-copy. Bare identity, bare str_split, routed terminal channels, and every declined projection
copy into the candidate. A non-null, unsliced string root whose projected channel feeds a codec
can skip the intermediate str_split/identity copy.

## 2. Post-merge retargeting (verified 2026-07-13)

A coworker merge (`785ce277` / `0880c7f1` / `a7389b68`, net −2137 lines) changed the remaining
anchors:

- `plan_compound` became `PlanTree`; `compressed_column::compound` is
  `std::unique_ptr<PlanTree>`, and `*col.compound` is the tree (no `.tree`). This affects C4, C5,
  and PR D candidate construction.
- The public consuming decompress overload did not exist at the merge baseline. PR C adds it and
  keeps `detail::apply_stored_dtype(..., col.dtype)` around the shared traversal.
- PR B's consumer counts, tombstones, labels, and `num_rows_hint` survived. No re-salvage is
  needed.
- The merge removed only direct STRING→ANS/bitcomp support. Routed numeric str_split channels
  still use the ordinary codec path, so PR A's production plans remain valid.
- The plan walk is still `ValueId`-keyed. PR D keeps
  `reprs_by_input`/`col_to_repr_key`/`repr_pending` and adds a small root-provenance map beside
  them; it does not add a second walk.
- `a7389b68` moved the JIT tree code but did not change C4's const codegen/bitjoin decode path.

## 3. PR sequence

Each PR must be independently buildable and ctest-green. Product-line budgets remain review
signals: exceeding one by more than 30% requires decomposition or an explicit design review, not
silent relabeling of code as documentation.

### PR A — pre-existing correctness fixes — LANDED `62802ad7` (budget net ≤ +110)

1. Reject mixed compressed/uncompressed chunk sets until E0 installs mixed-set support.
2. Fix `identity` plan spelling to `input -> identity` in the four affected sf1000 plans.
3. Route customer/supplier STRING identity columns through str_split, preserving v10.
4. Reject identity-STRING and any raw STRING leaf during header construction.
5. Check asynchronous staging copies and propagate their errors.
6. Catch view-path parallel worker exceptions instead of terminating.
7. Fix the INT32-offset assumption in both compressed-size calculators.

Gate already required the exact rerouted str_split shape through
`build_compressed_table_header` → `read_compressed_table_from_memory`, in addition to ctest.

### PR B — Step 0 internal copy/lifetime fixes — LANDED `d9641d22` (budget net ≤ 0)

- Move the reconstructed UINT8 payload buffer instead of allocating and copying it.
- Add memo consumer counts and consumed tombstones so a second destructive use fails loudly.
- Fabricate non-owning channel views for ANS/bitcomp/cascaded/nvcomp serialization; do not add a
  general borrowed-payload type yet.
- Move fixed-stride Raw passthrough output and keep RLE's compact path.
- Thread the v10 row count through reconstruction so bitpack does not perform a chunk-count
  device read.
- Document that fabricated channel views are usable only after their producer stream completes.
- Forward the caller's MR through `make_numeric_column`.

### PR C — Step 1 honest decompression — IMPLEMENTED, pending review (budget net ≤ +250)

- **C1 — lifecycle seam.** `take_decompressed` defaults to honest const `decompress`; identity
  and str_split override it. Observable operations on a consumed transferable rep throw one
  canonical error. Dictionary and codec reps remain reusable.
- **C2 — str_split storage.** Store offsets as a column and chars/mask as device buffers. Cache
  the exact chars dtype/element count and padded mask byte count; remove mutable move-through-
  const storage.
- **C3 — destructive ordering.** Resolve/validate first, mark consumed before moves, then build
  the strings column. The operation is intentionally not failure-atomic after transfer begins,
  but half-consumed state is never presented as reusable.
- **C4 — explicit traversal.** Add `consume_tag` and share one traversal between copy and take.
  Only reps already owned by the `PlanTree` honor the caller's policy; transient reconstructed
  reps are always consumed. Public rvalue single-stream decompression uses the tagged path.
  Rvalue thread-count and stream-pool overloads stay deleted until a correct executor exists.
- **C5 — tests.** Cover repeated const decode, identity/str_split pointer transfer and consumed
  errors, dictionary reuse, nullable v10 I/O fallback, widened chars descriptors, and worker
  exception propagation.

The old `reclaim_input` hook is intentionally absent: PR D never dismantles rollback ownership.

### PR D — Step 3 owned compression, single-stream — IMPLEMENTED AND AUDITED

Budget target: net ≤ +450 product lines. The original implementation report measured +698 and
therefore tripped the >30% review signal. The audit removed the placeholder/commit machinery and
shrunk the staged driver, but the isolated net must be recomputed from a clean PR C baseline before
submission; do not claim the target from a combined C + D working-tree diff.

#### D1. Transaction shell and public surface

Construct `staged_compression::impl` and move the original table into it before parsing plans or
building columns. The shell holds:

- the intact original `std::unique_ptr<cudf::table>`;
- a separate, fully owning `compressed_table` candidate;
- the captured CUDA device and borrowed stream;
- ready/failed state and a failure diagnostic.

`staged_compression` is move-constructible but neither copyable nor move-assignable. Its states
are:

- **ready** — `ok()`, inspection, accept, and reject are valid;
- **failed** — `error()` and reject are valid;
- **empty** — moved-from or decided; only `ok()`/`error()` are benign.

`table() const&` returns `compressed_table_inspection`, not a table reference. The facade hides
`columns`, reports shape, exposes the transaction stream, and pins `describe`, `decompress`, and
the inspection-specific header builder to that stream. Neither it nor payload pointers derived
from it may cross a decision.

#### D2. One walk, conditional projection, owning boundary

Thread an optional `staged_root_context*` through the existing plan walk. Null preserves the view
API. A staged context records root-component provenance by `ValueId` but owns no representation:

- identity may project its whole input;
- str_split may project offsets and chars from an intact root;
- a downstream materializing codec consumes those views and owns its output, ending provenance;
- a direct terminal or explicit downstream identity is copied to an owning identity leaf;
- a bare identity or bare str_split uses its ordinary copying compressor.

str_split declines projection for a real null mask, a sliced/head-sliced root, an empty column,
or chars wider than cuDF's `size_type` (>2 GiB). Those cases run the existing copying path. The
null-mask decline is required because the projection is entirely borrowed while the candidate must
own a terminal mask; v10 also serializes padding that cuDF does not define semantically, so the
ordinary copy path preserves existing behavior. Known zero-null nullable columns may still project
offsets/chars because no mask channel is emitted.

The candidate is self-contained before `table()` can expose it. Accept never releases a root
column, fills a placeholder, or runs a commit list.

#### D3. Decision, failure, and abandonment contracts

- `accept() &&` enters the captured device, synchronizes the stream at its current tail, moves
  the owning candidate, and destroys the original. Completion failure leaves the stage active.
- `reject() &&` performs the same proof, moves out the exact original table, and destroys the
  candidate. It works for ready and failed stages.
- Work launched from an inspection must use `inspection.stream()`; synchronizing the current
  tail includes payload reads queued after `table()`.
- A recoverable plan/operator/RMM allocation failure becomes a failed stage only after rollback
  quiescence is proved. Partial candidate output is cleared only after that proof.
- `cudf::fatal_cuda_error`, inability to establish rollback quiescence, and host
  `std::bad_alloc` escape. If construction throws, no rejectable result exists. The active stage
  destroys the input after quiescence, or deliberately retains it if safe destruction cannot be
  proved.
- An active stage's `noexcept` destructor enters the captured device and waits. If either proof
  fails, it emits a non-allocating diagnostic and deliberately retains/leaks the state rather
  than freeing storage that the GPU may still reference.
- The stage owns neither stream nor memory resources. The stage stream/MR and resources backing
  the original must outlive the active transaction; candidate resources must outlive an
  accepted table, and original resources a rejected table.

#### D4. Focused verification

`test_staged_compression` covers:

- facade immutability and transaction type traits;
- pointer-exact rejection for identity, bare/direct/routed str_split, and codec plans;
- self-contained accepted candidates with no source-pointer aliases and successful roundtrips;
- byte-identical v10 view/staged header and payload, including poisoned mask padding fallback;
- real asynchronous inspection-stream reads followed immediately by accept, reject, or
  destruction;
- unknown operators, multi-column partial failure, and limiting-MR injection;
- completion failures that leave accept/reject active, plus fatal rollback-quiescence failure;
- nullable, sliced, and empty projection decline; >2 GiB is covered by the same decline code but
  remains resource-gated rather than allocating a multi-gigabyte fixture;
- moved/decided misuse, multiple columns, and non-default-stream lifetime ordering.

Local verification on 2026-07-15 passed clang-format/pre-commit, `git diff --check`, the four
focused compression/I/O/ownership/staged tests, the Sirius extension build, Compute Sanitizer
memcheck, and Compute Sanitizer racecheck. Module CTest passed 11 of 12; the sole failure is the
known missing `cli/test/cli_roundtrip_test.sh` fixture and is baseline infrastructure, not a PR D
regression.

### PR E — Step 4 Sirius integration — FUTURE (budget net ≤ +250, two commits)

#### E0. Mixed-chunk support

Generalize the existing `insert_pinned_entry_{host,device}_compressed` functions to accept raw
chunks, compressed chunks, and logical order. Reuse `pinned_chunk_ref`, `logical_chunks`, and
`cached_databatch_provider`; do not add parallel `*_mixed` implementations. Fix order indices
across null `data_tables` slots, validate size, debug-assert a permutation, and remove PR A's
mixed-set guard only after the host/device behavioral tests pass.

E0 has no dependency on C or D and may be hoisted if useful.

#### E1. Stage, inspect, stage bytes, decide

Use the audited transaction in one shared helper:

1. Move the owned input into `try_compress_with_plan` on the same stream used to build/stage it.
2. Capture the read-only inspection and build the header through the inspection overload.
3. Apply the ratio decision while the stage is active.
4. Enqueue every payload read on `inspection.stream()` (or join other work into it).
5. Accept only after staging is ready; on ratio or recoverable staging failure, reject.
6. Install cache entries only after accept succeeds. Never retain the facade or a payload pointer
   across the decision.

If reject cannot prove completion, fail the pin loudly and let safe abandonment retain uncertain
storage. A host allocation failure before a stage can be returned is a documented loss window;
it must fail the pin rather than inspect a moved-from table.

Keep converter changes narrow: consuming decompress via `std::move`, deletion of the old subset
projection branch, and a defaulted selection parameter on the existing reader. Gate E with TPC-H
compressed-pin roundtrips on host and device tiers plus E0's mixed-chunk tests.

### PR F — Step 5 device-payload borrowing — FUTURE, optional

Land only after E, with the feature flag default OFF and profile evidence that the
compressed-byte-sized D2D is material. Reuse the validated meta decoder and a bind-or-copy leaf
source, but contain every borrowed payload view inside a one-shot
`decompress_device_payload(...)` call. That function itself must prove all payload reads complete
before returning owning columns; correctness may not depend on incidental per-column syncs.

PR F does not make `compressed_table` conditionally borrowing, does not change PR D's transaction,
and must pass memcheck/racecheck before any converter enables it. Payload alignment or metadata
changes remain wire-v11 work, not a quiet v10 edit.

## 4. If strict end-to-end zero-copy is required

The current PR D route is the correct general-purpose API for returning an owning
`compressed_table`: it removes intermediate decomposition copies where the plan shape permits,
but its terminal/bare leaves necessarily own their bytes. Bolting borrowed roots into that public
value would create conditional lifetime, constness, and failure contracts that do not fit the
type.

If profiling shows terminal copies dominate the Sirius pin path, design a sibling sink-bound
transaction after PR E proves the real consumer. A possible shape is
`stage_to_external_sink(...)` followed by `commit_external(...)`: header/payload construction can
read live root projections directly into storage owned by the cache/transport sink, and commit
that sink after completion. It must not first build today's owning candidate (the copy would
already have happened), and it must not return a borrowing `compressed_table`. The sink owns the
durable bytes; reject still returns the intact input. This is the ideal route for strict
end-to-end zero-copy, but it is a separate API backed by measurements, not a prerequisite for E
or F.

Other deferred work, in likely order:

1. **Pool/parallel owned driver** — a second executor on the intact-root core, with a real
   producer→worker event fence and streams/resources that outlive accepted allocations.
2. **Consuming parallel decompression overloads** — reuse C4's tagged traversal and worker error
   boundary before removing the deleted rvalue overloads.
3. **External-sink transaction** — only if E's profiles justify it, following the constraints
   above.
4. **Wire v11** — version dispatch and cross-version fixtures before metadata, mask, or payload
   alignment changes.

## 5. Attempt #1 defect ledger → disposition

| defect | disposition |
|---|---|
| `consume_tag` dropped, allowing const parallel decode to select a consuming overload | PR C restores the explicit tag. |
| `in_compound` recorded before fallible placement, creating a reject UAF window | Early-steal design rejected; PR D leaves roots whole. |
| Accept-time root detach depended on `column::release()` being genuinely no-fail | Rejected; candidates own all published bytes before inspection. |
| Production plans routed STRINGs to an unsupported identity leaf | PR A routes them through str_split; header rejection remains a backstop. |
| Dictionary `take_decompressed` copied and then poisoned reusable state | Dropped; the base take delegates without consuming. |
| `reclaim_input` could label a view-path copy as caller-owned | Hook removed; PR D never steals before a decision. |
| Take-walk moves could leave half-consumed reps | PR C validates and marks first, then moves; destructive decode is explicitly not failure-atomic. |
| Step 5 was enabled by default with unenforced cross-stream lifetime | Deferred to optional PR F with a one-shot completion contract. |
| Rollback invariant failure called `std::terminate` | No rollback reconstruction remains. Unprovable active-stage destruction retains storage. |
| Pin fallback had a null-table hole and logical-order skew | PR E1 and E0 respectively. |
| Timing-based adoption-fence test | Removed with the pool driver; a deterministic event test is required when that executor returns. |
| Tests hard-coded v12 offsets/version | Not ported; v10 remains frozen. |
| Unscoped environment mutation and duplicated helpers | Use scope guards and shared test helpers. |

One non-defect remains documented: attempt #1's whole-file formatting hunks match the repository's
pre-commit clang-format hook. Touched files should follow the hook rather than preserve artificial
minimal diffs.

## 6. Footprint accounting and landing gate

Attempt #1 was **+4,343 / −587 over 43 files**. Excluding lockfile/tests, it was approximately
+3,040 gross / +2,450 net product lines, including about 870 lines of Step 5, 270 lines of v12
wire work, and 400+ lines of duplicate drivers and validation walls.

The planned product budgets remain A ≤ +110, B ≤ 0, C ≤ +250, D ≤ +450, E ≤ +250. PR F and a
possible external-sink API are separate, profile-gated budgets. Tests are accounted separately.
The reported pre-audit D size (+698 net) exceeded its gate; before submission, isolate C and D,
measure each against the correct parent, and either bring D within the review threshold or record
an explicit approved exception. Comments that specify ownership, failure, and stream contracts
are product safety work, but they do not disappear from the raw metric.

Final C + D gate:

- clang-format/pre-commit and `git diff --check`;
- focused C/D ownership and staged-compression binaries;
- existing compression plan, header/I/O, and decode roundtrips;
- module ctest with the missing CLI fixture classified against baseline;
- extension build;
- memcheck and racecheck on the staged transaction tests;
- a clean diff with no `.orig`/`.rej`, generated artifacts, or unrelated changes.
