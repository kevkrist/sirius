# S4 publication review — finalization

## Final reviewer verdict — 2026-10-23

**Ready for the final four-GPU validation handoff. No blocking finding remains.**
Production source, cleanup, fixtures and schema-v2 harness are approved within
the scope below. Both planned local SQL comparisons pass. The additional Q1
oracle blocker is closed: its narrow test-side correction, negative controls,
protocol/artifact identities and retests through all three revisions are verified.
The inspected worktree is `/home/ubuntu/sirius/build/s4-final`. This is not
four-GPU performance acceptance.
The reviewer has not run builds, tests, GPU commands or child agents and changes
only this report. Publication and source reconstruction belong to the parent.

The three review scopes are:

- Full remaining production against prerequisite commit
  `78a3e34c66143152ff075729f4c018806c11e67d`, tree
  `9f59d68e4cdc7ca47999c3579407f6f5168ea652`.
- Removal of the rejected pilot against screened S4 control
  `9b2e8201787f381b62715c01405f659b925c49a7`. This comparison assesses cleanup;
  it cannot establish the overall effect of S4 against its prerequisite.
- Schema-v2 native fixtures, test-only baseline overlays, build helper,
  runner/analyzer, CPU tests and executable handoff guides. Tracked and new
  files are included. The final worktree starts at `d925db5d`; that commit alone
  does not contain the reviewed cleanup.

[S4_FINALIZATION.md](S4_FINALIZATION.md) supplies the acceptance boundary.
The historical first-handoff review below remains evidence about its original
source and runs; its former pilot approval does not describe final production.

### Production and lifetime assessment

Final production has no concurrent-ingress selector, getter, extra ingress
streams or copied-event schedule. Compile commands contain one Bloom
implementation translation unit per revision, with no S4 production definition.
Revision-role and baseline-compatibility definitions apply only to the fixtures.

Removing the allocation-ready event is safe: scratch allocation and ingress
copies use the same publication stream; the root stream waits on ingress-done
before the OR. The global chunk sequence preserves alternating-half reuse across
keys, and ingress waits for the previous merge before overwriting that half.
Egress waits for the merge; publication completion joins root and target work.
The task stream and partial streams remain in the pre-reserved drain set.
Pipeline, staging and scratch owners outlive failure draining; an unjoinable
failure retains the complete owning state. The bounded HOST path synchronizes
its staging reuse and completes replication before return. Recoverable fallback
remains before mutation; fatal CUDA failures are not converted into admission
refusals. Accurate existing ownership comments are preserved.

The constructor-fault skip of six and delayed-record skip of two match the
remaining event construction/record sequence. The test-only wrappers exercise
the owned submission boundary without duplicating the transfer algorithm.
No speculative abstraction or additional test is required by this review.

### Native, harness and artifact assessment

Both reference overlays contain only the four shared native fixture files,
including the oracle header, the baseline CMake module and the exact test-gated
root include. Baseline
production is unchanged. The prerequisite reports unsupported S4 publication
metrics as null, even when its real common publication count is positive. Its
older final-contribution latency is never substituted for S4 preparation time.
Equal-work query timings remain comparable without that telemetry. Changed
publication, contribution, admission or filter outcomes are reported and excluded
from overhead ratios. CPU parity and effective-profile/interception checks remain
mandatory. Builder and transport observations are final-only diagnostics.

The analyzer retains independent process-pair inference, separate baseline noise
controls and the per-query 3% guard. Two-GPU screens cannot satisfy that guard.
Schema-v1 evidence is preserved and uses its original committed analyzer.

Independently inspected both frozen pair manifests:

- `build/s4-final-pair/build-manifest.json`: exact prerequisite versus final.
- `build/s4-control-artifact/build-manifest.json`: exact screened control versus
  the same final manifest and artifacts.

The actual fixture, library, extension and CLI hashes match every manifest.
Manifest/source/configuration digests recompute correctly, and current source
diffs match the recorded diffs. Dependency identities and shared SQL fixture
hashes agree. Normalized configuration is identical, with SHA-256
`1656f1c976965c7a27d9255fdfe67e5c7ae51c589688ca3eb32dc2820623873e`.
The original strict-oracle final source identity is
`bb40f2f103b7295996f1993eb0a01550255b18878fea72814332f07fd84d0bc9`.
Saved loader records resolve each extension to its own frozen library. The
prerequisite campaign's actual CLI smokes also completed: both logs contain the
success marker and initialize their matching frozen libraries, with `-bail` and
`-no-agent` in the recorded commands.

### Fresh validation inspected

These are parent-run results, independently read from the persistent final or
prerequisite `build/s4-local-validation/` directories; they are not reused `/tmp`
results from the first handoff.

| Check | Observed result |
| --- | --- |
| Prerequisite focused suite | 89 cases / 5,576 assertions passed |
| Prerequisite isolated faults | 4 cases / 228 assertions passed |
| Final focused suite | 94 cases / 6,970 assertions passed |
| Final isolated faults | 7 cases / 835 assertions passed |
| Original strict-oracle harness CPU checks | 28 tests passed |
| Corrected-oracle harness CPU checks | 30 tests passed |
| Focused Q1 comparator checks | 6 cases / 43 assertions passed |
| Final publication fault memcheck | 3 cases / 607 assertions; zero reported errors |
| Final late-cancellation memcheck | 2 cases / 54 assertions; zero reported errors |
| Native direct and forced-HOST memchecks | Both exit zero, zero reported errors |

Six valid native cases passed the complete bitmap and membership oracles: direct
roots 0/1, HOST roots 0/1, local ordinal 1, and reordered devices 1,0 with a
noncontributing replica. They include one/two keys, tails and cross-key half reuse.
The direct/HOST memchecks use root 1, two keys and 8,388,864 bytes per key;
`native/memcheck-results.json` records explicit API checking and exit zero.
The rejected noncontributing-root request is retained separately and is not a
successful runtime case. Source hook evidence inspected covers C++/CMake.
`harness-doc-hooks.log` also records passing Black, Python debug-statement and
codespell checks. Its rumdl hook reports that it modified files, followed by
`Success: No issues found in 3 files`; this is formatting-fix evidence, not a
clean all-hooks-pass invocation. The parent separately reports the three agent
guides passing rumdl.

The prerequisite SF100 campaign is complete in
`build/s4-local-validation/prerequisite-sf100`, with analysis in
`build/s4-local-validation/prerequisite-analysis.json`. Independently verified the
completion hashes, all 12 planned processes, 48 unique records and 36 measured
samples. Aggregated records exactly match native files; each process supplies one
warmup and three measured repetitions. All records pass CPU parity, GPU
interception and effective-profile checks. Build manifests and the three harness
source hashes match the frozen artifacts. Query/preparation SQL and configuration
hashes match their recorded identities.

Q4 has no accumulated publication in either revision. Q21 reports three
publications and thirteen completed contributions; final publishes 174,974,624
bitmap bytes. Q7 reports one publication and two completed contributions; final
publishes 23,982,720 bitmap bytes. Both positive final cases use direct transport.
Prerequisite publication timings and bytes remain null. All common outcomes
match in every measured revision pair and baseline control; no changed-work pair
is excluded. Independently recomputed query ratios and medians agree with the
analyzer. Publication ratios are correctly excluded because the prerequisite
lacks that telemetry. All comparisons remain exploratory one-cluster results,
with no confidence interval and no all22 acceptance pass.

The separate frozen-control campaign and `control-analysis.json` are also complete
under `build/s4-local-validation/`. Independently verified its completion hashes,
12 planned processes, 48 unique records and 36 measured samples against untouched
native payloads, excluding only the added `harness` metadata. Every record passes
CPU parity, GPU interception and profile checks. Common outcomes match every
measured pair and noise control; independent query ratios agree with the analyzer.
Its CLI smoke logs initialize the correct libraries and contain the success
markers. Both comparisons reuse the same final manifest, dataset identity and
configuration; the control campaign uses seed 1824.

Control comparison query median deltas are Q4 -0.279%, Q21 +0.595% and Q7 +0.293%;
maximum absolute baseline-control median drift is 1.155%. These are one-cluster
observations without an interval, not a non-regression conclusion. Positive Q7/Q21
publication intervals are comparable between these two S4 revisions; Q4's absent
publication is correctly excluded. Across both planned campaigns, all 96 records
(72 measured) pass, with no changed-work exclusions for query timings.

### Resolved Q1 oracle blocker

The additional Q1 SF100 preflight failed with exit code 1 using the original frozen
final fixture. This was outside the planned Q4/Q7/Q21 performance comparisons and
blocked handoff until the correction below. Independently read
`q1-preflight/request.json`, `native.jsonl`,
`stderr.log` and `exit-code.json` under `build/s4-local-validation/`; the fixture
hash still matches its manifest. The sample reports `cpu_gpu_result_mismatch`,
four result rows, distinct CPU/GPU digests, successful GPU interception, verified
profile and no accumulated publication. The subsequent process-error record is
not another measured sample.

The parent subsequently ran a diagnostic through the frozen CLI. Independently
read its request/SQL, `types.csv`, CPU output, two GPU outputs and
`q1-diagnostic/differences.json`, and recomputed the cell differences. Both GPU
outputs are identical. Only `avg_disc`, declared DOUBLE, differs for keys `(N,O)`
and `(R,F)`: decimal text differences are 1e-17, roughly 2e-16 relative, and the
parsed binary64 values differ by one ULP. All other 38 cells match exactly,
including keys, BIGINT counts, DECIMAL sums and the other averages. The diagnostic
exited zero. This evidence supports correcting the fixture's exact-string oracle
for floating-point precision; it does not require a production edit.

Additional strict preflights of Q8, Q14 and Q17 each exit zero with matching
CPU/GPU digests, successful interception, profile checks and oracle results.
Independently read `float-preflight/results.json`, the three native records and
requests; each uses the original frozen final fixture, one measured execution
and no warmups. These are oracle-operability checks, not additional performance
campaigns, and supply no justification for a general floating-point tolerance.

The architect's quantified correction permits approximation only for Q1's
`avg_disc` at index 8, of type DOUBLE, after recognizing the expected ten-column
names/order, matching complete types/shape and exact unique leading group keys.
Every other value, including other Q1 averages, remains exact; NULLs, one-to-one
row identity and multiplicity remain protected. Eligible finite numbers must
parse completely and satisfy relative tolerance 1e-12 or absolute tolerance
1e-12. Nonfinite values match only through exact equality.

Independently reviewed the frozen shared helper, native call sites/metadata,
focused comparator tests and the single CMake test-source registration. The
source remedy establishes the contract with no blocking finding. Complete
schema/shape validation precedes indexing. Exact key sorting plus duplicate-key
rejection preserves row identity. The existing `cells_equal` helper receives only
the two non-NULL column-8 values after Q1 recognition; it rejects partial numeric
parses and nonfinite mismatches. Every other cell remains exact. Snapshot and
comparison occur outside the query timer; original digest inputs and algorithms
are unchanged. Production file hashes match the previously reviewed cleanup.

The actual red-stage log has six cases and 43 assertions: the observed-roundoff
case and two within-tolerance positive assertions fail, while all 40 remaining
assertions pass. The preserved red header differs from the frozen helper in the
small acceptance loop, helper include, policy constants and documentation. Tests
cover observed values with reordered rows, other workloads/columns, a count
increment at 1e15, DECIMAL/other-AVG changes, schema, NULLs, shape, duplicate keys,
nonfinite/partial parses and values outside both tolerance bounds.

The green comparator log now passes all six cases and 43 assertions.
Independently verified identical overlay bytes in all three worktrees: four shared
native files, including the oracle header, plus the baseline CMake module. The
existing gated root include remains the only baseline build registration change.

The frozen runner delta also has no blocking finding. It hashes the oracle header
as part of the shared fixture identity and validates the three fixed policy fields.
Partial or altered policies fail; paired identifies and successful SQL samples
must agree. Both historical strict records lacking these fields remain supported,
while mixing strict and corrected identities fails. Original digests may differ
for accepted Q1; no digest-equality gate was added. New CPU controls exercise
policy changes, omissions, mixed identities and differing accepted digests.
Independently read `oracle-harness-tests.log`: all 30 tests pass. The separate
`oracle-source-hooks.log` passes its applicable C++, Python and CMake hooks;
Markdown was not selected in that invocation.

The analyzer and build helper are unchanged. Independently verified that all
three restored files under `measured-harness/` match the original campaign's
recorded SHA-256 values. Independently audited the new pair manifests under
`build/s4-final-pair-oracle/` and `build/s4-control-artifact-oracle/`: actual artifact
hashes, manifest/source/configuration digests, current source diffs, untracked
source hashes, all four shared fixture hashes and dependency identities match.
Both pairs reuse the same final manifest and the original matched normalized
configuration. All identifies bind the fixed oracle policy/tolerances. Corrected
final source identity is
`41961b96c7679094d7f565d74a30f60cce1d436ac7bae6f74ddd8e1419fae5ac`.

The refreshed shared library for each revision has the identical SHA-256 of its
original strict-oracle library, independently checked against both artifact sets
and `oracle-library-proof.json`. The change is test-side.

The current runner's `q1-corrected-campaign` completes all four planned processes
and four samples, with zero warmups and one repetition per process. Independently
verified its completion hashes, exact native/aggregate payload correspondence,
planned identities, refreshed build manifest, current harness hashes and both
actual CLI loader smokes. Every prerequisite/final record passes CPU parity under
the fixed policy, effective profile and GPU interception. Its analysis reports
only exploratory observations, no confidence interval or acceptance guard.

The separate `q1-control-corrected` native run also exits zero. Its one record,
request and exit-code artifact bind to the refreshed control manifest and exact
policy/tolerances, using the same configuration and Q1 SQL/preparation files.
Oracle, profile and GPU interception all pass. All five corrected Q1 records retain
the original distinct CPU/GPU digests and four result rows, with no accumulated
publication. Thus Q1 passes through prerequisite, frozen control and final without
forging exact equality or changing production. These five records are additional
oracle/protocol checks, not another performance campaign. The blocker is closed.

The failed preflight and strict Q4/Q7/Q21 campaigns remain under their original
manifests; earlier hashes identify those strict-oracle artifacts, not the correction.
The local two-GPU configuration and footer-count evidence are recorded in
[S4_LOCAL_FINALIZATION_VALIDATION.md](S4_LOCAL_FINALIZATION_VALIDATION.md).
Positive Q7/Q21 publication on the acceptance host, four-GPU memory behavior,
all22 non-regression and the separate cleanup comparison's performance guards
remain remote acceptance work. Bounded
memcheck does not establish race freedom or emulate an unjoinable device loss.

### Handoff boundary

The local evidence is packaged in
[20261023T182408Z-local-finalization](docs/experiments/s4-results/20261023T182408Z-local-finalization/),
with `local-evidence.tar.gz`, `archive-files.json`, analyses, audit records and
`SHA256SUMS`. The parent verified all 1,017 archived members' sizes/hashes and
extracted them into `build/s4-evidence-audit/`. Independently verified the ten
listed outer checksums and inspected the inventory and written
`archive-audit.json`.

Independently compared the saved per-campaign reanalysis outputs against the
original prerequisite, control and corrected-Q1 reports: each contains its one
intended campaign, and the only differing value is `campaigns[0].directory`.
The successful reproduction used one analyzer invocation per campaign, with the
preserved strict harness for the original comparisons and the current harness
for corrected Q1. This is evidence reproduction, not a new GPU test or performance
run. The archive preserves original manifests, caches, commands, raw evidence and
the strict campaigns' measured harness bytes; binaries and datasets are excluded.

The reviewed guides provide the exact prerequisite/control references, test-only
overlay, fresh remote builds, final-only diagnostics, paired all22/cleanup commands,
analysis and evidence-return procedure. Existing user authorization covers the
specified personal-fork result commit. The parent owns snapshot packaging,
verified fork publication and the concluding architect step. This review makes
no claim that publication or remote execution has already occurred. No further
source remedy or additional local production test is required for this handoff.

### Finalization reading refresh

Reused this reviewer's exact personal book/Core Guidelines scopes recorded below.
Freshly read the reviewer role, AGENTS, Document Code skill and complete cuDF
syntax reference; refreshed full Super Sirius README, architecture overview,
memory management and query lifecycle. Refreshed dynamic-filter sections 71–145
and 248–277, and multi-GPU sections 98–111, 183–200 and 216–258. Read the full
finalization contract, accepted experiment report, current guides and changed
developer/fixture/harness notes, plus the actual source and evidence described
above. Dependency refresh covers the full RMM stream module and cuCascade memory
sections 1–106, 194–289 and 360–392; pinned sources remain authoritative.

Additional substantive refresh: complete Core Guidelines R.1, R.12, E.6, E.16,
E.19 and CP.23; complete Software Design Guideline 4 and Guideline 2's
premature-separation discussion/summary; complete supplied C++20 sections 5.6,
7.3 and 23.8.1. These are exact selected scopes, not new full-book claims.

## Historical first four-GPU handoff review

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
