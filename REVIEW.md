# S4 publication review — ready for first four-GPU handoff

Comparison: frozen prerequisite tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652`
against tracked S4 changes and new files in `/tmp/sirius-s4-publication`. HEAD and
staged prerequisite integration are not the S4 diff. Scope is readiness for the
user's four-GPU handoff, not selection of a performance winner.

**Verdict: ready for the first four-GPU experiment handoff.** No blocking
correctness, lifetime, standards, specification or measurement-contract finding
remains in the inspected implementation. R1 is fixed and verified in the compiled
pilot. Both schedules, focused/fault tests, bounded memcheck and the actual paired
runner passed the local checks detailed below. This verdict authorizes no topology
winner, four-GPU speedup claim, or final S4 performance acceptance.

Review includes the full tracked delta and new fixture, build-helper, Python and
handoff files. The reviewer changed only this report, launched no builds/GPU tests
or child agents, and independently inspected the parent-produced artifacts/logs.

## Standards

The original R1 violated the lifetime requirements behind Core Guidelines R.1/E.6:
a borrowed CUDA handle must remain owned through the operation using it. Its source
fix is verified below. No style-only or speculative abstraction findings are recorded.

## Resolved spec finding

### R1 — P2, closed: constructor failure left destroyed ingress streams in the drain list

**Original defect proof.** In the first-pass version of
`src/cuda/sirius_dynamic_bloom_filter.cu:703–710`, the concurrent
`chunk_pipeline` constructor creates an ingress stream and immediately appends its
borrowed handle to the caller-owned `joined` vector. Construction can still throw
while creating that source's copied events, another source's stream/events, or a
target's completion event. C++ then destroys the partially constructed object's
stream owners; assignment to `pipeline` at lines 1190–1198 never completes. The
outer catch nevertheless passes the unchanged `joined` vector to `join_on_failure`
at line 1288. It calls `cudaStreamSynchronize`/`cudaStreamQuery` on destroyed streams.
This introduces invalid CUDA calls during preflight failure and can replace a
recoverable metadata refusal with a fatal cleanup error. Source control flow proves
the defect; no injected runtime reproduction is claimed.

**Required property.** Every handle visited by failure draining must still have a
live owner. All streams that can receive submitted work must be registered before
their first submission; a failed, submission-free constructor must not leave stale
registrations.

**Cheapest credible remedy.** Construct the pipeline completely, then append its
ingress handles to the already-reserved drain list before `order_after_inserts`.
Alternatively roll back only the constructor's appended handles on failure. No
general stream registry is needed. Verify a construction failure after the first
ingress stream exists: cleanup must avoid destroyed handles, preserve the original
failure classification, and retain/drain the existing task/partial work.

**Remedy status:** source fix independently verified. The constructor no longer
mutates `joined`; `register_ingress_streams(joined)` runs after successful
`unique_ptr` construction and before `order_after_inserts`. The existing
`reserve(1 + 2*N)` covers the task stream, N partial streams and at most N-1
ingress streams. Every registered stream therefore has a live pipeline owner,
including if subsequent work throws. **R1 is closed:** the correctly identified
pilot's complete isolated-fault suite passes (7 cases, 835 assertions), including
the late event-creation failure after its first ingress owner exists. Its test
asserts fault consumption, no invalid join, unchanged partials, no copy submission
and restored allocation accounting. The control fault suite also passes.

## Source assessment

- HOST fallback allocates one physical fixed-block window, rounded to the root
  HOST resource's block size, and at most one root-device chunk. It uses the supplied
  task allocator without another reservation, reuses the existing partial arrays,
  joins D2H/H2D/OR before reuse, and does not restart after submission or mutation.
- The pilot owns one root-device ingress stream per non-root contributor. Initial
  reads follow source insertion and scratch allocation; root OR follows every
  copied event; ingress reuse waits for the previous merge of that half; egress
  follows the current merge before overwriting source bytes. The global sequence
  spans keys. Host submission binds each wait before that event's next record.
  The completion barrier transitively joins ingress, root merge and every egress.
- The final drain list includes task, partial and successfully constructed ingress
  streams. Failure draining precedes workspace release. Unjoined failure retains
  the pipeline/stream owners, HOST allocation, scratch and whole builder. Native
  CUDA failures propagate rather than becoming ordinary allocation refusals.
- Input bytes count contributing GPU bitmaps; ready bytes count every completed
  replica; channel-published bytes count a key once across accepting bindings.
  Preparation-through-completion time remains distinct from final-contribution-to-
  first-push latency. Outcome recording precedes active-operation settlement.
- The builder fixture computes a CPU OR of every byte of every private partial
  before publication, then checks every ready replica and membership. Transport
  uses real owning pools and separate source/destination storage per edge. SQL
  compares complete type/null-aware row multisets with duplicate multiplicity;
  digest comparison alone is not its oracle. Timed normal SQL has separate CPU
  oracle execution and transparent interception/counter evidence.
- Separate compiled schedules, static-fixture identity, matched shared artifacts,
  frozen inputs, process pairing, baseline/baseline controls and cluster-based
  intervals are explicit. Screen samples cannot establish a selection interval;
  missing publication is excluded from publication improvement. Unavailable local
  event times, allocator peaks and physical routes are not invented.
- Focused tests cover exact byte equality, chunk tails/half reuse across keys,
  contributor/root variants, HOST rounding/exhaustion, task reservation accounting,
  delayed copies/events and post-submission failures. The isolated fault and
  corrected normal suites pass for both compiled schedules. Runtime
  evidence is recorded below; source review alone is not treated as a test pass.

## Other corrections tracked during integration

- Parent found the private schedule definition was applied in the wrong CMake
  directory. `TARGET_DIRECTORY sirius_objects` now fixes its source scope.
  Independent inspection of `build/release/compile_commands.json` confirms exactly
  one production CU has `SIRIUS_S4_CONCURRENT_INGRESS=1`, namely
  `src/cuda/sirius_dynamic_bloom_filter.cu`; both frozen fixture identify records
  report their intended schedules.
- Parent reproduced native SQL preparation failure on an internal `SET` option.
  Source confirms the cap and multi-partition options are registered only when
  `SIRIUS_ENABLE_TEST_OPTIONS=1`; the coverage option is public. This is **not** a
  YAML-only API. The agreed minimal fix is the existing test-option environment
  guard before database/extension initialization, with actual setting checks and
  a paired normal-SQL smoke. No new production setting is required.
  The native guard, runner environment and effective session-profile check are
  now independently source-verified. Parent-produced q4/q21 tiny-data records
  (`/tmp/sirius-s4-sql-q4-smoke-internal.jsonl` and
  `/tmp/sirius-s4-sql-q21-smoke.jsonl`) show CPU-oracle success and GPU interception;
  both honestly report zero accumulated publications and null publication timing.
  Those preliminary records do not establish paired-build publication performance.
- Initial isolated fault execution did not intercept three batch-error and two
  batch-delay cases: cuDF makes the CUDA batch call from its shared library, beyond
  executable link wrapping. The corrected test wraps the existing out-of-line
  `sirius::cuda::device_copy_batch::enqueue` boundary instead. Source and object
  symbols independently confirm the caller's undefined reference and the exact
  method definition. The wrapper preserves the const object/stream arguments,
  delegates success to the real method, and injects the same native status into
  production's checked return path. Delayed-success cases enqueue the delay on
  that stream before invoking the real copy; failure cases leave delayed stream
  work for the production drainer. No transfer algorithm or production hook was
  added. The mangled symbol is intentionally tied to the pinned Linux/CUDA ABI;
  both link failure and the test's `consumed()` assertion expose a future mismatch.
  This tests batch-boundary failure with pending work, not an actual CUDA batch's
  partially accepted descriptor prefix. Independently read corrected parent log
  `/tmp/sirius-s4-current-fault-tests-2.log`: **7 cases, 835 assertions, all passed**.
- The first current-direct normal run ended at 94 cases, 92 passed and 2 failed
  (6,866 assertions, 3 failed). Both failures are in the old pipeline scratch-OOM
  expectations: one assumes the channel stays empty after direct scratch refusal;
  the other assumes every scratch request has the direct size and only one request
  occurs. The new implementation correctly tries smaller HOST-path scratch. The
  required test correction checks successful fallback without task retry and
  retirement when both workspace requests are refused. No production change was
  justified by those failed assertions. The test-only correction is source-
  verified: generated cases separately assert smaller HOST success/full membership
  and both refusals/no publication; both assert two workspace requests, zero task
  retries, normal deposits, released reservations and worker reuse. The refusal
  case checks allocation recovery, and the standalone retirement test checks the
  ordered direct-size/root-chunk requests outside its throwing allocator callback.
  Corrected control execution is independently verified in
  `/tmp/sirius-s4-control-final-tests.log`: **94 cases, 6,970 assertions, all passed**.
- Paired-build validation initially rejected a wrapper resolving the prerequisite
  library through a stale `sirius_DIR` CMake cache entry. The helper's minimal
  `EXT_FLAGS` addition, `-Usirius_DIR`, clears that entry on every wrapper configure;
  the existing Makefile supplies the current install as `sirius_ROOT`. Source and
  rerun log confirm both wrappers relink. The helper also copies the already-built
  isolated fault executable beside each schedule's unit executable. The rejected
  bundle remains `build/s4-rejected-wrapper`; it is excluded from accepted evidence.

## Final local validation

Independently read the final control normal and corrected isolated-fault logs:
94/6,970 and 7/835 respectively, both all passed. Parent reports formatting/lint
on all S4 paths and Markdown checks on nine files passed. HEAD remains S3c
`9c329e31f65be938bf9de4fc823bd29abf0359cc`; the index comparison against the frozen
prerequisite tree remains empty. No S4 commit or staging is attributed to this
review. The delivered helper completed the pair under `build/s4/`; successful
rerun log `/tmp/sirius-s4-paired-build-2.log` ends with both per-schedule manifests
and `build/s4/build-manifest.json`. Independently inspected manifests establish:

- intended static-core schedule identities and distinct fixture/library hashes;
- identical source SHA256
  `57f35e0ab8fac84b353387be33432641d5477f773287f58286abdf1b05d4eb9e`;
- identical schedule-normalized configuration SHA256
  `6e1fbaccf2a89a597b2a3dcc9495c6ed4069d1227bde830873e0ba94bec7fabe`;
- each extension resolves its own schedule's frozen `install/lib/libsirius.so.0.0.0`.

Read-only SHA256/size recomputation matches all four manifested binaries and the
CMake cache for each schedule. ELF dynamic sections independently show each
extension's schedule-specific install RPATH. Both frozen fault executables exist;
they are copied validation artifacts, not members of the four-binary measurement
manifest.

Independently read `/tmp/sirius-s4-candidate-tests.log` (**94 cases, 6,970
assertions, all passed**) and `/tmp/sirius-s4-candidate-fault-tests.log` (**7 cases,
835 assertions, all passed**), the latter naming the frozen concurrent-ingress
executable. These establish pilot execution separately from the control.

The first four entries of `/tmp/sirius-s4-sanitizer/results.json` record successful
memcheck exits for each compiled schedule's `require_direct` and `force_host`
builder runs: two GPUs, root 1, K=2, B=8 MiB+256, one repetition. Each associated
log has `ERROR SUMMARY: 0 errors`, and each JSONL has successful exact bitmap and
membership oracles. The commands use `--report-api-errors explicit` and
`--error-exitcode 86`; these are bounded memcheck results, not a racecheck or
four-GPU claim. The completed results file also records candidate fault and
cancellation memcheck exits of zero: `candidate-fault.log` passes 3 cases/607
assertions and `candidate-cancellation.log` passes 2 cases/54 assertions, both with
zero errors.

The actual paired runner completed `build/s4-results/local-smoke`, with log
`/tmp/sirius-s4-paired-smoke.log` and analysis
`build/s4-results/local-analysis.json`. It ran transport, builder and SQL stages
on GPUs 0,1, default256, explicitly labeled `unknown/tiny` data, Q4/Q21/Q7, one
warmup and one measured repetition per process. Parent reports runner/analyzer
exit zero. Independent bundle inspection verifies:

- 50 planned processes and 100 unique records: 50 warmups and 50 measurements.
  All records have `status=ok` and passed oracles. The records comprise 28
  transport, 48 builder and 24 SQL samples, including warmups.
- Every aggregate record equals its saved native record after removal of the
  harness metadata. Every process request, schedule, build ID and pairing identity
  matches its plan. Eighteen baseline/candidate processes, eighteen baseline/control
  processes and fourteen transport diagnostic processes are present. Paired arms
  share their workload/seed; baseline/control uses current-direct in both arms.
- Completion hashes match the saved experiment and samples. Frozen SQL/config
  identities, dataset content hashes and captured source diff/new-source hashes
  match the inspected inputs and implementation.
- All 48 builder records pass complete bitmap equality and membership. All 24 SQL
  records pass CPU parity, effective-profile verification and GPU interception.
  Every SQL record reports zero accumulated publications, zero published bitmap
  bytes and null publication time. Those SQL observations establish the runnable
  normal-SQL path, not a measured accumulated-publication benefit.
- Both CLI smoke requests use `-bail -no-agent`, complete their load marker and
  show loader initialization of their own frozen schedule's shared library.
  This is extension compatibility evidence; the native SQL fixture separately
  exercises its statically linked production objects.
- Analysis includes all 50 measured samples, 14 successful local Stage A samples,
  exploratory screen assessments and baseline noise controls. It reports no
  selection confidence interval, no all22 regression pass and no topology winner.

Independently read `/tmp/sirius-s4-harness-final-tests.log`: 20 CPU harness tests
passed. These are separate from the native GPU results. The executable handoff
commands and experiment/confirmation protocol are in [S4_EXPERIMENTS.md](S4_EXPERIMENTS.md).

## Acceptance boundary

This completes reviewer acceptance of the inspected tree and paired artifacts
for the **first** four-GPU experiment handoff. No further code remedy or speculative
abstraction is required. The subsequent architect step remains the parent's
coordination responsibility.

Local runtime evidence uses two GPUs. It does not exercise multiple simultaneous
non-root ingress streams on four GPUs, establish SF100/SF1000 publication gains,
run the held-out confirmation/all22 performance guards, or select/prune the pilot.
Those are the user-run experiment stage. Actual physical routes, local stage-event
durations and allocator committed peaks remain unavailable. Memcheck does not
replace racecheck, and the injected failure tests do not emulate an unjoinable
device loss or native partially accepted descriptor batch. These explicit limits
do not block the bounded first handoff. Parent source-archive reconstruction is
separate packaging work; this verdict does not claim to have checked an archive
that has not yet been supplied.

## Exact reviewer reading record

Reused this reviewer's actual prior substantive readings, not another agent's
summary and not whole books:

- *C++ Software Design*: complete chapters 1 (Guidelines 1–5) and 3 (11–14).
- *C++20 — The Complete Guide*: complete supplied sections 5.3, 5.6, 5.7,
  7.2–7.4, 18.1 and 23.8.1. The supplied text includes draft markers; this is
  neither a complete chapter-5/chapter-7 nor a complete-book claim.
- *C++ Core Guidelines*: F.53; C.13, C.31–32, C.36–37, C.41–42;
  R.1, R.3–5, R.12, R.20–21, R.30; E.1–8, E.12–19, E.28;
  CP.2–4, CP.20–26, CP.31–32, CP.42–44, CP.50, CP.60.

Fresh preparation read `/home/ubuntu/.codex/agents/reviewer.toml`, worktree
`AGENTS.md`, Document Code's full `SKILL.md` and
`references/cudf-documentation.md`, Code Review's skill and module-context's
workflow. User instructions override the Code Review skill's child-agent step;
both review axes were performed personally. Read full worktree
`S4_ARCHITECTURE.md` (291 lines at preparation),
`S4_IMPLEMENTATION_KICKOFF.md` (119), and `S4_BASELINE.md`.

Super Sirius refresh: full baseline README, architecture overview, memory
management, query lifecycle and unified dynamic-filter design; selected complete
sections in `dynamic-filters.md` lines 1–134 and 244–321, implementation plan
121–219, and multi-GPU architecture 98–111, 183–200 and 216–258. Read the relevant
CONTRIBUTING code-style/submodule, reviewability and configuration sections.
Later reviewed the S4 dynamic-filter documentation delta and all S4 developer,
fixture, harness and experiment notes available during this pass.

Dependency preparation: cuCascade memory module lines 1–144, 194–289 and 360–392;
full RMM CUDA-stream module. Actual cuCascade sources at
`1a9281093a0fe49634594d541d7ce327129f53d6` govern: fixed HOST resource header
45–205/243–334, implementation 94–202, and reservation adaptor 385–535.
Traced baseline builder partial/pipeline/finish ownership, publisher admission/
contribution/cancellation/retirement, pool grants, replica reservations, stream
owners, batched copies, geometry and SQL snapshot helpers. The subsequent actual
review read the complete S4 diff against the frozen tree plus the new files;
prerequisite/index changes were excluded. No runtime or full-book reading is
inferred from source inspection.
