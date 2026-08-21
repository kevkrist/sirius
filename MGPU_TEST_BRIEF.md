# Mission: multi-GPU validation of branch `kk/batch-preview-mgpu` (test-only — do NOT modify code)

Context: this commit is a 14-fix batch on the dynamic-filter publication subsystem
(eager publication claim, Bloom-budget rescoping, partition scheduling/locking fixes,
sync-outside-lock contribution, telemetry). Single-GPU tags all pass on the dev box;
every multi-GPU leg is untested because `[mgpu]` tests self-skip below 2 visible GPUs.
The review cycle has since approved the batch with test/doc-only nits, so the
**production code here is final** — results on this branch are valid evidence for what
ships. Your job is to execute the multi-GPU legs and report evidence, not to fix
anything.

## Setup

```bash
git submodule update --init --recursive
pixi run make
```

Run tests one process at a time — they assume exclusive GPU ownership.

## Test invocations (sequential, in this order; save each full output)

```bash
UT=build/release/extension/sirius/test/cpp/sirius_unittest
pixi run $UT "[mgpu]"                                      # whole multi-GPU suite
pixi run $UT "[dynamic_filter][publication_claim]"          # 2-GPU-gated tests activate here
pixi run $UT "[dynamic_filter][publisher]~[integration]"    # accumulator cross-device legs
pixi run $UT "[dynamic_filter]~[integration]"               # full unit sweep on this hardware
pixi run $UT "[integration][dynamic_filter]"                # needs the TPCH integration DB if configured
```

## Highest-value targets and their failure signatures

1. `physical_hash_join - broadcast BUILD_PROBE publishes dynamic filters across two GPUs`
   — exercises the new eager publication claim under real racing broadcast deliveries.
   RED FLAGS: missing `dynamic-filter publication:` in the log dir; any
   `dynamic filter NOT published` line (a losing delivery saw an open window it
   shouldn't); assertion on double publication.
2. `a whole build resident on a non-plan GPU reopens the window` (publication_claim)
   — its attempt-counter expectations were flipped for the eager claim (1→2) and have
   NEVER executed on any machine. A counter mismatch here is the most likely genuine
   failure.
3. Accumulator cross-device tests (`merge_from` / strict replication / mgpu reservation)
   — first run under the new submit-under-lock/sync-outside contribute. RED FLAGS:
   hangs, or lost inserts (mask mismatches).
4. Partition scheduling tests — the task-issue path was restructured
   (barrier-check-first, narrowed lock). RED FLAG: any test hang/timeout is a finding
   even if rare.

## Race hunting

The claim and contribute changes are concurrency fixes; single passes prove little.
Re-run these 10x and count failures:

```bash
for i in $(seq 10); do pixi run $UT "[mgpu][dynamic_filter]" || echo "FAIL iter $i"; done
for i in $(seq 10); do pixi run $UT "[dynamic_filter][publisher][concurrency]" || echo "FAIL iter $i"; done
```

## Optional (if TPC-H data is configured on this box)

Run a handful of join-heavy queries (q5, q7, q21) GPU vs CPU with
`SET sirius_log_level='debug'` and a log dir; grep the logs for
`dynamic-filter publication:` / `dynamic filter NOT published` and confirm results
match CPU. Report the publication-line counts per query.

## Report back (exact format)

- Per invocation: pass/fail with Catch2's final counts line, wall time.
- Any failure: full test output verbatim + the log-dir contents it references.
- Race loops: N failures / 10, with outputs for each failure.
- GPU count/models, driver version (`nvidia-smi` header).

Do not attempt fixes, reruns-until-green, or code edits — a flaky pass hides a finding.
