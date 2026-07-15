# Zero-copy string compression/decompression in simpatico — staged root projection

- Status: **reviewed design; contracts corrected; PR C and the audited PR D implementation are
  present and verified locally**. Sirius integration and optional device-payload borrowing remain
  future work.
- Scope: simpatico engine API + Sirius pin/scan integration, branch
  `compression-subsystem-port`.
- **Out of scope: `.hpln` persistence to disk.** Header plus payload is an in-process transport
  between pin and scan in one binary. If disk persistence returns, it needs version dispatch and
  cross-version fixtures; it is not implied by this design.
- Objective: remove redundant STRING decomposition copies where an intact owned root can be read
  directly by a materializing codec, without changing the view API, the fixed-width/JIT path, the
  frozen v10 wire, or the eager-release walk.
- Contract authority: this document defines the architectural contract. The companion
  [implementation plan](zero-copy-implementation-plan.md) sequences the work. The public headers
  remain the source of truth for exact C++ signatures.

## 0. Design verdict

The staged owning entry point is the right API boundary, and root projection is the right narrow
extension to the existing walk. The caller transfers an owned table into a move-only transaction;
the walk may borrow scoped root views while it constructs codec outputs; and `reject()` can return
the exact original table because that table is never dismantled.

The earlier accept-time detach design was not sound. It relied on terminal placeholders plus
`cudf::column::release()` after the transaction's point of no return. In the pinned cuDF version,
`release()` is declared `noexcept` but allocates host-side buffer shells. Host OOM can therefore
terminate rather than unwind. There is no public allocation-free primitive that can detach all
STRING components into a candidate. A transaction cannot honestly promise a no-fail commit on
that basis.

PR D consequently provides **conditional copy elision**, not universal zero-copy:

- eligible root channels that feed downstream materializing codecs are borrowed directly;
- every candidate leaf is owning before the candidate is exposed;
- bare identity/str_split representations and projected terminal channels use the ordinary copy;
- `accept()` does not patch, detach, allocate, or reconstruct the candidate.

This is not scaffolding forced onto the wrong abstraction. The owning transaction and the
root-only projection seam fit the current engine well. What would be the wrong fit is making an
accepted `compressed_table` conditionally borrow from a destroyed transaction, or spreading an
owned/borrowed variant through every interior value solely to optimize roots.

For strict end-to-end zero-copy in the Sirius pin path, the ideal future boundary is a transaction
that commits directly into an external header/payload sink (for example, `commit_external`). Such
a sink can copy raw terminal bytes from the intact root straight to the final memory tier and
return an external-store descriptor, rather than first manufacturing an independently owning
`compressed_table`. The host/device tier transfer remains real data movement. This future API is
complementary to, not a replacement for, the safe general-purpose owning result in PR D.

### Book grounding

The design follows the principles in the repository's `C++ Software Design.pdf` (printed page /
PDF-viewer page):

- Guideline 6, 44–52 / 64–72: an abstraction is a set of behavioral expectations. A staged result
  must not appear independently owning until it actually is, and failure effects must be explicit.
- Guideline 8, 56–61 / 76–81: operations with materially different semantics deserve distinct
  names. The owning transaction therefore supplements rather than silently changes the view API.
- Guideline 9, 63 / 83: the transaction example supports an explicit all-or-recover decision.
  It does not justify a fallible operation after the point of no return.
- Guideline 22, 177–179 and 184–185 / 197–199 and 204–205: value semantics make ownership and
  outcomes easier to reason about; borrowed inspection remains scoped and explicit.
- Guideline 23, 190–191 / 210–211: an rvalue otherwise binds to `const&` and copies. This supports
  the separate consuming decompression overload, but does not prove rollback semantics.
- Guideline 34, 345 / 365: non-owning wrappers belong at argument/view boundaries, not as hidden
  persistent state in an owning return value.
- Guideline 2, 23 / 43: design for the change that is actually required. Root provenance is kept
  narrow rather than turning every walk value into an ownership framework.

## 1. Design summary

Compression is a move-only staged transaction with one explicit decision:

```text
try_compress_with_plan(unique_ptr<table>, plan, stream, mr)
    -> ready:  intact original + complete self-contained candidate
    -> failed: intact original + error; no candidate is inspectable

ready.table()        -> immutable inspection bound to the stage stream
move(ready).accept() -> prove completion, discard original, return candidate
move(active).reject() -> prove completion, discard candidate, return exact original
```

The important boundary is **before inspection**: a ready stage's candidate already owns every byte
it exposes. Root projection is only a construction-time optimization. Payload descriptors obtained
through inspection may point into the candidate, never into a root that acceptance still needs to
detach.

If `try_compress_with_plan` returns, the stage owns the original and every partial or complete
candidate allocation. Recoverable compression failures are values represented by a failed stage,
but only after stream quiescence has been established. This is not a total C++ function: host
allocation failure while capturing the stage or a fatal CUDA condition may escape after the
by-value owner has transferred.

`accept()` and `reject()` synchronize the stream's current tail. If completion cannot be proved,
the decision throws and leaves the stage active and unchanged. Successful rejection is
allocation-free and performs no device copy or kernel; it moves the still-whole original table
back.

Inside the plan interpreter, only root provenance is new. The ordinary view API, JIT fusion,
interior representation lifetime/refcounts, and eager release are unchanged. Decompression remains
an honest overload pair: the `const&` path is repeatable and copying; the `&&` path may transfer
identity/str_split storage and is expressly not failure-atomic.

## 2. Public contracts

### 2.1 Owning compression and inspection

The public shape is intentionally narrower than a `compressed_table const&` observer:

```cpp
class compressed_table_inspection {
 public:
  std::size_t num_columns() const noexcept;
  std::int64_t num_rows() const;
  rmm::cuda_stream_view stream() const noexcept;
  std::vector<std::vector<leaf_desc>> describe() const;
  std::unique_ptr<cudf::table> decompress(device_async_resource_ref mr) const;
};

class staged_compression {
 public:
  staged_compression(staged_compression const&) = delete;
  staged_compression& operator=(staged_compression const&) = delete;
  staged_compression(staged_compression&&) noexcept;
  staged_compression& operator=(staged_compression&&) = delete;
  ~staged_compression() noexcept;

  bool ok() const noexcept;
  std::string const& error() const& noexcept;
  compressed_table_inspection table() const&;
  [[nodiscard]] compressed_table accept() &&;
  [[nodiscard]] std::unique_ptr<cudf::table> reject() &&;
};
```

`compressed_table_inspection` hides `compressed_table::columns` and therefore the public
`unique_ptr<PlanTree>` objects. This prevents shallow-const mutation of a live transaction. Its GPU
operations are pinned to the stage stream; callers cannot accidentally describe or decompress on
an unrelated stream. The facade is copyable but borrowed and must not outlive its stage.

The header builder has an inspection overload with no stream parameter:

```cpp
build_compressed_table_header(
  compressed_table_inspection,
  std::vector<std::uint8_t>& header,
  std::vector<payload_buffer_ref>& buffers,
  std::uint64_t& payload_bytes);
```

Returned payload pointers remain valid only while the stage is active. Copies from those pointers
must be enqueued on `inspection.stream()` so the later decision covers them.

The existing `compress_with_plan(cudf::table_view, ...)` overloads stay unchanged. Retain-the-input
callers still need them; the owning transaction is a distinct verb because it has different
ownership and failure semantics.

#### Transaction states (normative)

A stage has exactly three states:

- **ready** — the complete self-contained candidate and intact original both exist;
- **failed** — an error and intact original exist, and no candidate is inspectable;
- **empty** — moved-from or successfully decided.

Only ready and failed are active. `ok()` is true exactly in ready. `error()` contains the reason
only in failed and is empty in ready/empty. `table()` and `accept()` require ready. `reject()`
accepts either active state. Misuse throws before ownership changes. The move constructor transfers
the whole state and leaves the source empty; move assignment is deleted so it cannot silently
abandon an active decision. The type is not thread-safe.

Ownership transfers when the by-value `unique_ptr` parameter is initialized. The caller must not
retain or use views into the moved table. A null input becomes a failed stage whose rejection
returns null, provided the stage shell itself can be constructed.

#### Failure classes (normative)

- **Recoverable compression failure:** plan/data validation, unsupported operation, operator
  exception, codec refusal, and RMM device allocation failure. The implementation first proves
  rollback quiescence, clears partial owning candidate state, and returns a failed stage.
- **Fatal compression failure:** `cudf::fatal_cuda_error`, inability to prove rollback
  quiescence, and unrecoverable device/context loss escape. They are never disguised as
  `ok()==false`.
- **Host allocation failure:** `std::bad_alloc` escapes because constructing a reliable failed
  state may itself require host allocation. If an exception escapes, there is no rejectable stage
  and the by-value input is destroyed.
- **Decision/completion failure:** `accept()` or `reject()` cannot establish completion. It throws
  with both outcomes still active and unchanged.
- **Contract misuse:** use after move/decision, inspecting a failed stage, invalid
  stream/resource lifetimes, or violated input ordering is outside rollback guarantees.

No caught failure may free candidate or original storage until all enqueued work that could refer
to it is known complete.

#### Device, stream, resource, and destruction contracts (normative)

The stage captures the current CUDA device at creation. Recovery synchronization, decisions, and
destruction re-enter that device before touching device state. This makes a later caller-side
`cudaSetDevice` change harmless; destruction still occurs in the allocation context.

All prior asynchronous reads and writes involving the input must happen-before work on the supplied
stage stream. No other stream may continue using the transferred storage unless its work is joined
into the stage stream first. Inspection-triggered reads and payload copies must use the inspection
stream. Decisions wait at the stream's **current tail**, so work enqueued after `table()` is
included.

The stage owns neither its stream nor any memory resource. The stream, the candidate resource, and
resources backing the original must outlive the active stage. Candidate resources must outlive an
accepted table; original resources must outlive a rejected table. RMM buffers retain their
allocation/deallocation stream metadata, so accepted storage can also require the supplied stream
to outlive it. PR D is therefore deliberately single-stream.

Destroying an active stage abandons both outcomes; it does not implicitly accept or reject. The
`noexcept` destructor enters the captured device and proves current-tail completion before freeing
storage. If either proof fails, it emits a non-allocating diagnostic and intentionally retains the
state rather than deallocating buffers the GPU may still use.

### 2.2 Decompression overload pair

```cpp
std::unique_ptr<cudf::table> decompress(compressed_table const&, stream, mr);
std::unique_ptr<cudf::table> decompress(compressed_table&&, stream, mr);

std::unique_ptr<cudf::table> decompress(compressed_table&&, int, mr) = delete;
std::unique_ptr<cudf::table> decompress(compressed_table&&, stream_pool&, mr) = delete;
```

The const overload remains genuinely non-consuming and repeatable. The rvalue overload calls the
mutable `decompress_column(PlanTree&, consume_tag)` path. The tag is load-bearing because dereferencing
a const `unique_ptr<PlanTree>` is shallow const and could otherwise select a mutable overload.
Codegen and bitjoin traversal remains const/copying.

`take_decompressed()` defaults to ordinary `decompress()` and does not poison a representation.
Only identity and str_split transfer stored buffers and become consumed. Dictionary and other
representations remain reusable even through the rvalue table overload. Transferable reps validate
metadata before marking consumed, then mark immediately before their first move. A later allocation
or factory failure may leave the source partly consumed; this is part of the public rvalue contract.

Consumed identity/str_split reps reject repeated take/decompress and serialization access with one
canonical `logic_error`. Parallel const workers surface exceptions to the caller rather than
terminating.

### 2.3 Root projection seam (engine-internal)

The ordinary compressor remains view-based. A root-only hook is available to the staged driver:

```cpp
enum class root_component { whole_column, offsets, chars };

struct projected_root_channel {
  compressible_output output;  // borrowed while the intact stage-owned root lives
  root_component source;
};

struct staged_root_projection {
  std::vector<projected_root_channel> channels;
  std::vector<std::string> required_channels;
};

struct staged_root_context {
  std::unordered_map<ValueId, root_component, ValueIdHash> provenance;
};

struct compressor {
  virtual std::unique_ptr<compressed_representation>
  compress(cudf::column_view, stream, mr) = 0;

  virtual std::optional<staged_root_projection>
  project_staged_root(cudf::column_view, stream, mr) const;
};
```

Only identity and str_split project. A projection is not a representation and owns nothing. It is
considered only for a root-backed value whose node declares output channels; a bare operation is
terminal and takes the ordinary owning/copying path. Projected views may feed downstream
materializing compressors, and the context records which resulting `ValueId`s remain root-backed.

The provenance map sits beside, rather than inside, `ValueColumnMap`. A root-backed value has no
representation/refcount owner, so ordinary `release_column` merely drops its view. Intermediate
representations keep the existing eager-release rules. Provenance is validated at each projection:
a projection of an already decomposed component cannot claim unrelated root components.

If a projected value reaches a terminal, the driver immediately copies it into an owning identity
leaf. Thus no root-backed view, placeholder, commit target, or conditional owner can escape in the
candidate. A null staged context is the old view path, including its early return for bare
representations; it does not enumerate lazy channels or introduce new allocation behavior.

## 3. Ownership model

**One regime, one sentence:** *the stage keeps every original root whole; root projections may
lend scoped construction inputs, but a ready candidate owns everything it exposes.*

| root/plan shape | staged construction | accept after completion | reject after completion |
|---|---|---|---|
| eligible projection, all channels feed materializing codecs | codecs read root views directly and own their outputs | discard original; return complete candidate | discard candidate; return exact original |
| projected channel reaches a terminal | copy terminal into an owning identity leaf during construction | discard original; return complete candidate | discard candidate; return exact original |
| bare identity or bare str_split | ordinary owning compressor copies its stored bytes | discard original; return complete candidate | discard candidate; return exact original |
| projection unavailable or declined | ordinary owning/copying compressor path | discard original; return complete candidate | discard candidate; return exact original |
| fused/JIT or other materializing root | unchanged view-based materialization | discard original; return complete candidate | discard candidate; return exact original |

There is no accept-time component transfer. Acceptance consists of entering the captured device,
proving current-tail completion, moving out the already-owning `compressed_table`, and destroying
the original. The only fallible operation precedes the ownership change. Rejection proves the same
completion condition, moves out the untouched table, and destroys candidate state.

### 3.1 Identity projection

Identity may project the whole input view when it is a routed construction step. If that routed
value feeds a materializing codec, the codec can read the original root directly. If it is terminal,
the walk copies it immediately. A bare `input -> identity` is itself the stored representation and
therefore takes the ordinary owning copy path.

This distinction is intentional: an independently owning `compressed_table` cannot preserve a bare
identity root without either copying it, dismantling the original, or retaining a hidden rollback
owner. The first is the only safe option with the current cuDF API.

### 3.2 str_split projection and fallback

str_split projects only the offsets and chars channels, and only when all of these conditions hold:

- the input is a non-empty STRING column;
- it is unsliced (`offset()==0`) and its offsets child covers exactly `size()+1` entries;
- it has no logical nulls (a nullable column with known zero nulls is eligible);
- chars fit in the normal cuDF fixed-width element-count limit (at most `INT32_MAX` bytes).

The projection performs the scalar `chars_size` query, then lends offsets and chars views to
materializing downstream codecs. No null-mask channel is projected.

The ordinary copying str_split compressor is the mandatory fallback for:

- actual nulls: the all-borrowed projection cannot supply an owning terminal mask, and
  serialization includes padding that cuDF does not define semantically; the ordinary path
  preserves existing v10 behavior;
- sliced or head-sliced strings: offsets/chars must be rebased into a consistent representation;
- empty strings columns: the projection has no useful child storage to lend;
- chars above 2 GiB: the copy path widens the chars element type and zero-initializes trailing
  padding that a borrowed view cannot prove initialized.

The current copy path rejects chars beyond its documented larger bound. The resource-heavy >2 GiB
arm is not normally exercised in unit tests; decline-to-copy behavior is covered by the other
ineligible shapes and the size guard remains explicit.

### 3.3 Parallel owning driver — deferred

PR D is single-stream. A future per-column pool executor would need an exception boundary in every
worker, an explicit producer-to-worker event fence, and stream/resource ownership that outlives all
accepted allocations. Timing-based tests are not a substitute for the fence. Root retention does
make per-column rollback conceptually straightforward, but it does not solve those lifetime rules.

### 3.4 Decode-side payload borrowing — optional later phase

A future `decompress_device_payload(header, device_payload_base, ...)` may contain
parse → borrow → decode → completion → return within one engine call. Borrowed leaves may feed JIT
or nvCOMP calls, but every returned column must own its output and the function must prove payload
reads complete before returning. No borrowed payload state may escape. Pinned-host payload still
requires H2D. This is profile-gated work because it removes only a compressed-bytes-sized D2D copy
and needs payload-alignment and cross-stream lifetime tests.

## 4. Copy ledger

For an eligible non-null canonical STRING plan
`str_split; offsets -> delta -> bitpack; chars -> lz4`, let `U = O + C` and
`R = comp(O) + comp(C)`.

| movement | ordinary/view path | staged eligible path |
|---|---|---|
| str_split offsets/chars decomposition | about **U** D2D plus synchronization | **0** D2D; one scalar `chars_size` query |
| materializing codec output | **R** plus codec scratch | unchanged; inherent codec work |
| bitpack/lz4 compaction | compressed-size movement | unchanged; inherent format construction |
| serialized payload exposure after the existing fabricated-view fix | no extra D2D | no extra D2D |
| final pinned-host/device-store staging | one memory-tier transfer | unchanged; required transport |
| undecided root storage | caller-owned input remains elsewhere | original **U** retained as rollback arm |

For nullable data with actual nulls, sliced/empty strings, or >2 GiB chars, str_split falls back and
the decomposition copy remains. For a bare identity or bare str_split terminal, the candidate also
copies the retained bytes. The design does not call either case zero-copy.

The fixed-width JIT path is unchanged: it reads the same raw pointer through the same columns map;
only the stage owns the root object. The ordinary view API follows its original branch with a null
staged context.

On ratio rejection or recoverable failure, successful `reject()` performs zero device bytes and
zero kernels: it discards candidate work and returns the exact original allocations. Completion
failure is excluded because returning possibly in-use memory is forbidden.

## 5. Migration and verification

### Step 0 — internal fixes, no API change

- release nvCOMP output buffers instead of copying them where the representation already owns the
  allocation;
- move decode memo outputs only under a proven single-consumer count, with reuse after move a loud
  runtime error;
- expose codec serialized buffers through fabricated non-owning views while the representation
  owns them, and reject sizes beyond the v10 field limit;
- move, rather than duplicate, the fixed-stride raw passthrough where ownership is exclusive.

These changes are independent of the staged transaction and retain fixed-width benchmark gates.

### Step 1 — honest decompression (PR C)

Provide repeatable `const&` decompression and explicitly consuming `&&` decompression. Transfer only
identity/str_split storage, make consumed-state access loud, preserve immutable stored views, and
keep rvalue parallel overloads deleted until a safe mutable worker path exists. Verify pointer
transfer, repeated const decode, consumed access, unknown null counts, widened chars, JIT/bitjoin,
and v10 in-memory round trips.

### Step 2 — frozen wire v10

Do not bump the version or change payload layout for PR C/D. Routed str_split reconstruction uses
`null_count=-1` when a mask exists and resolves it from the mask; known non-nullable input uses
zero. Device-payload alignment is a future format decision and does not retroactively change v10.

### Step 3 — staged owning compression (audited PR D)

- capture the intact original, candidate, CUDA device, stage stream, and error in a move-only stage;
- share the ordinary plan walk and add only a nullable staged root context/provenance map;
- project identity/eligible str_split roots only into downstream materializing codecs;
- copy all bare and terminal storage so the candidate is self-contained before inspection;
- expose immutable stream-bound inspection and a matching header-builder overload;
- classify device OOM as recoverable only after rollback quiescence, while fatal CUDA and host OOM
  escape;
- synchronize current-tail work on accept/reject/destruction under the captured device;
- retain storage instead of unsafe deallocation when a `noexcept` destructor cannot prove safety.

Verification covers exact-allocation reject for ready and failed stages, candidate non-aliasing,
view-vs-staged wire equality, poisoned null-mask padding, nullable/sliced/empty projection fallback,
multi-column partial failure, deterministic allocation-failure injection, completion-failure state
preservation, fatal rollback-quiescence escalation, current-tail payload work, moved/decided misuse,
and non-default-stream lifetimes. The >2 GiB allocation itself remains resource-gated.

### Step 4 — Sirius integration

Build the header and stage payload bytes through `compressed_table_inspection`, using only
`inspection.stream()`. Keep the transaction alive until payload staging has completed, then accept.
On ratio rejection or recoverable staging failure, reject and recover the exact input. If rejection
cannot prove completion, fail the pin operation loudly and let the active stage take its safe
abandonment path; never publish a pinned entry whose source lifetime is uncertain.

Capture row count/allocation accounting before moving the owner. Selection-aware reads, mixed-chunk
policy, STRING identity handling, valid plan spelling, and checked CUDA copy returns remain hard
integration preconditions.

### Step 5 — strict external commit and device-payload borrowing (deferred)

If profiling justifies strict pin-path zero-copy, design a sink-oriented commit that consumes a live
stage and writes root-backed terminals directly to the final external payload store. Its result
should describe that store, not masquerade as an owning `compressed_table`. The sink protocol must
make completion, partial writes, cancellation, and resource lifetimes explicit.

Treat direct device-payload borrowing as a separate opt-in change with sanitizer and cross-stream
coverage. Neither future feature should weaken the safe PR D owning contract.

## 6. Related integration defects to keep visible

These defects were surfaced by the broader verification work and are not reasons to weaken the
transaction contract:

1. Some generated plan files use bare `identity` blocks that the parser rejects, causing Sirius to
   fall back to uncompressed pinning. Regenerate them as valid `input -> identity` plans and log
   plan failures loudly.
2. STRING identity serialization must describe offsets/chars/mask components, not pretend the
   chars pointer begins one contiguous offsets-plus-chars allocation; otherwise reject the shape
   loudly.
3. Sirius payload staging must check every CUDA copy return code.
4. Parallel view compression needs worker exception propagation rather than `std::terminate`.
5. Decode memo reuse after move must be a hard error, not an implicit re-decode.
6. STRING size accounting must honor INT64 offsets for large strings.
7. Serialized reads should project columns before fetching all leaves.
8. Compressed leaves beyond the v10 size field limit must fail loudly rather than truncate.

Each item should be independently verified against the current branch before editing; line numbers
from earlier audits are intentionally not treated as stable contracts.

## 7. Rejected alternatives

- **Accept-time terminal placeholders plus `column::release()`.** Rejected because the public cuDF
  release path can allocate host shells inside `noexcept`. That is not an allocation-free commit
  primitive and can terminate after the point of no return.
- **Early steal followed by rollback reconstruction.** It destroys the strongest rollback object,
  adds stolen/in-compound states and harvest ordering, and still cannot make a fallible detach an
  honest no-fail decision.
- **`variant<owned, view>` through every walk value.** Ownership originates only at roots. A
  parallel root-provenance map localizes the actual variation point.
- **Allowing an accepted `compressed_table` to borrow the original.** This creates a hidden
  conditional lifetime and violates the owning result's expected behavior.
- **Keeping the entire original inside an accepted result.** It preserves pointers but defeats the
  memory objective and changes the meaning of compressed ownership.
- **Describing terminal copies as zero-copy.** They are required by the current owning boundary and
  remain in the copy ledger. A future external sink is the honest route to removing them for the
  pin use case.
- **Replacing the view API with the consuming API.** Retain-the-input callers still need the view
  form; the transaction is an additional operation with different semantics.
- **Making ratio policy part of simpatico compression.** Ratio acceptance belongs to Sirius, and
  external staging failure still needs a reversible transaction.
- **Letting decoded tables borrow payload storage.** A contained one-shot device-payload function
  can prove completion; a borrowed owning table cannot.
- **Claiming the by-value sink is total.** A host failure before the stage shell exists cannot
  return the moved owner. If that guarantee becomes necessary, add a separate in/out-owner API and
  clear it only once transaction capture succeeds.
