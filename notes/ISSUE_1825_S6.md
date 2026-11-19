S6: Activate eligible probes after build layout and filter completion
Deliver the manifest's early probe-production optimization when build partition layout is stable, relevant dynamic-filter producers are terminal, and memory admission preserves build progress.

Tracking: [umbrella](https://github.com/sirius-db/sirius/issues/1818) | [design and implementation plan](https://github.com/sirius-db/sirius/pull/1819). Depends on [S5](https://github.com/sirius-db/sirius/issues/1824) in the agreed linear stack.

Planned head: `kevkrist/sirius:df/06-early-probe`. Local branch parent: `df/05-domain-evidence`. Upstream PR target: `sirius-db/sirius:dev`. Draft upstream diffs are cumulative until predecessors land; link the immediate-parent fork comparison. The chosen sequence remains unchanged unless the implementation plan is explicitly updated.

Own PARTITION layout-completion wiring, hash-join scheduling hints, and integration with existing task requests and memory admission. [PR #1451](https://github.com/sirius-db/sirius/pull/1451) remains reference material and need not merge or change.

Acceptance:

- [ ] Retain normal deposited-build readiness as an alternative to early activation: `normal_build_deposit_ready || (layout_stable && target_producers_terminal && memory_admitted)`. Actual hash probing still waits for its partition's built table.
- [ ] Wire layout readiness from the explicit build-PARTITION completion source. Require terminal rights from all relevant endpoint producers, including unrelated producers and empty/skipped/failed/plan-dropped outcomes; filter presence is not completion.
- [ ] Subscribe before execution using subscribe-and-recheck or equivalent level-triggered behavior, and reevaluate on both layout completion and last-producer terminal notification plus normal deposits. Coalesce requests through the existing task-request mechanism.
- [ ] Notify only after releasing caller/operator/session/channel locks, including notifications caused by session begin under PARTITION's first-pop lock. Use weak query/task targets and invalidate them before teardown destroys operators.
- [ ] Preserve normal progress for absent completion sources, unsupported graph shapes, and producers in the probe subtree; no new blocking channel wait or dependency cycle is introduced.
- [ ] Admit buffered early probe output through existing reservation/backpressure rules without consuming build-progress capacity. A terminal no-filter result does not itself justify early work; retain normal scheduling when safe admission cannot be demonstrated.
- [ ] Validate the real build/CONCAT/probe graph, both notification orders, unrelated/probe-subtree producers, all-skipped/zero-filter terminals, narrowing, cancellation/teardown, missed-wakeup prevention, and pressure from buffered probe output.
- [ ] Include this layer's scheduling docs, observability, and internal ablation controls without adding a public mechanism-selection knob.

Delivery and final-stack evidence:

- [ ] Developer implements; reviewer audits liveness, lock/notification ordering, teardown, and memory progress; developer resolves findings in S6.
- [ ] Record cumulative build evidence and exact test commands; coordinate GPU tests/benchmarks with the user and distinguish proposed checks from results and skips.
- [ ] Complete a cumulative review of C1 and S1-S6, fold each defect into its owning layer, restack affected descendants, and record repository-required test/lint and agreed multi-GPU/CPU-oracle outcomes before final acceptance.
- [ ] Validate all six optimizations together across useful/ineffective filters, pinned/native/unpinned scans, direct/fallback transport, and memory pressure. Record commits, cuCascade pin, hardware/environment, skips/failures, and per-optimization ablations; treat the campaign's `814.2 -> 595.0 ms` seven-query profile as prior evidence and a reproduction target, not a promised result.
- [ ] Link the implementation PR/revision and cumulative evidence here; mark the umbrella complete only when C1 and S1-S6 have landed with required validation. This planning issue claims no implementation or test execution.

