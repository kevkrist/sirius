Plan: complete-build dynamic filters and six-optimization delivery stack
Implement the architect-reviewed complete-build dynamic-filter design as six Sirius PRs, including all six optimizations from the 2026-09-17 campaign manifest and a separately merged cuCascade prerequisite.

## Source of truth

- Architecture: `docs/super-sirius/dynamic-filters-unified-design.md`
- Delivery/dependency plan: `docs/super-sirius/dynamic-filters-implementation-plan.md`
- Reviewed delivery plan: https://github.com/kevkrist/sirius/blob/6ef3e7d19e2741f0c50566a19c9667854a281cad/docs/super-sirius/dynamic-filters-implementation-plan.md
- Reviewed architecture: https://github.com/kevkrist/sirius/blob/6ef3e7d19e2741f0c50566a19c9667854a281cad/docs/super-sirius/dynamic-filters-unified-design.md
- Design-only PR: https://github.com/sirius-db/sirius/pull/1819
- Existing #1451 supplies implementation/test reference material. The new PRs implement the revised design; merging #1451 is NOT a prerequisite. Leave #1451 unchanged.
- Related background: #1620. This effort does not claim to resolve every deferred item in that issue.

## Delivery sequence

| Layer | Scope | Dependency |
| --- | --- | --- |
| [S1 — #1820](https://github.com/sirius-db/sirius/issues/1820) | Owned one-shot publication session, identified producer completion, coherent channel snapshots | Current upstream dev |
| [S2 — #1821](https://github.com/sirius-db/sirius/issues/1821) | Complete pre-scatter inventory, retry-safe multi-partition accumulation, private Bloom builders and peer-only chunked publication | S1 |
| [S3 — #1822](https://github.com/sirius-db/sirius/issues/1822) | Shared scan consumer, applied-entry receipts, one survivor gather and fused membership masks | S2 |
| [S4 — #1823](https://github.com/sirius-db/sirius/issues/1823) | Sirius H02 pool-access integration, direct-path validation, and required host-staged publication fallback | S3 AND the separately merged cuCascade API |
| [S5 — #1824](https://github.com/sirius-db/sirius/issues/1824) | Copied pinned-domain usefulness evidence and measured coverage profile | S4 in this review stack |
| [S6 — #1825](https://github.com/sirius-db/sirius/issues/1825) | Optional early probe activation with explicit completion notifications and preserved build progress | S5 in this review stack |

## cuCascade prerequisite

Prerequisite issue: https://github.com/NVIDIA/cuCascade/issues/199

Open a separate PR in NVIDIA/cuCascade from a branch in kevkrist/cuCascade. It provides the reviewed per-owner/accessor pool-grant result API and a borrowed reservation-adaptor pool accessor, with library-owned tests. Adapt the real campaign delta to current cuCascade main; exclude the driver shim and campaign ancestry.

S4 must wait for that PR to merge upstream, then update the Sirius gitlink to the merged commit or a reviewed descendant. It must deliver and validate H02 and the chunk path on capable hardware. Runtime serial fallback is required but does not substitute for that delivery.

## GitHub workflow

All Sirius PR heads come from kevkrist/sirius; the planning/task issues live here. Fork branches form a linear local stack, but upstream PR bases are dev. Native GitHub stacks do not support cross-fork stacks. Later upstream drafts therefore have cumulative diffs until predecessors land; include parent-to-child compare links for incremental review. Restack onto refreshed sirius-db/sirius/dev after each merge, not a stale fork dev.

S1-S6 are native sub-issues of this umbrella, with native blocked-by relationships for each predecessor and S4's cuCascade prerequisite. These relationships track work; they do not enforce a PR merge restriction. S4's implementation PR must remain draft/not merge-ready until C1 is merged upstream and its pin/H02 validation gate is met. Open implementation PRs when their code is reviewable; creating these tasks does not deliver implementation.

## Acceptance

- [ ] https://github.com/NVIDIA/cuCascade/issues/199 — C1: required cuCascade API PR reviewed, merged and linked.
- [ ] https://github.com/sirius-db/sirius/issues/1820 — S1: lifecycle and channel contract delivered.
- [ ] https://github.com/sirius-db/sirius/issues/1821 — S2: complete-inventory accumulation and peer-only chunked publication delivered.
- [ ] https://github.com/sirius-db/sirius/issues/1822 — S3: scan-on-view and fused mask optimizations delivered.
- [ ] https://github.com/sirius-db/sirius/issues/1823 — S4: merged dependency, H02, validated pipelined publication, and host-staged fallback delivered.
- [ ] https://github.com/sirius-db/sirius/issues/1824 — S5: domain-evidence optimization delivered.
- [ ] https://github.com/sirius-db/sirius/issues/1825 — S6: early activation delivered.
- [ ] Developer -> reviewer -> fixes completed per layer, plus cumulative review.
- [ ] Required test/lint, multi-GPU and CPU-oracle evidence recorded on exact revisions.
- [ ] Full six-optimization performance profile and ablations remeasured.

Preserve production coverage default 0.9 and explicit measured profile 0.45. Caller unique_cols assertions remain performance hints, separate from join-execution uniqueness proofs. The campaign's seven-query 814.2 -> 595.0 ms result includes H02; it is prior evidence, not a claimed result for the redesign.

No implementation has been delivered by creating this planning issue. GPU test runs must be coordinated before execution.



## Scope change (2026-09-29)

S2 (#1821) now also does the piece-by-piece filter copy that S4 (#1823) planned, and it skips the filter instead of using a slower fallback when two GPUs cannot copy directly. S4 keeps the direct access to each other's memory pools and the follow-up measurement. See the notes on #1821 and #1823.

## Required S4 fallback (2026-09-30)

S4 (#1823) must restore serial reduction and replication through pinned host memory when direct GPU-to-GPU transfer is unavailable or cannot be established for the actual allocation pools. Lack of direct access alone must not disable multi-partition dynamic filters; inability to safely acquire fallback resources may still decline the optional filter. Validate actual fallback publication and correctness as well as the direct path. This resolves the previously deferred fallback decision: it is required in S4, not S3, and does not add a cuCascade prerequisite beyond C1.



## Merge topology follow-up (2026-10-05)

S4 (#1823) also takes the measured merge-topology follow-up: measure concurrent peer bandwidth, run the merge's inbound copies in parallel, and split the reduction across GPUs only if the measurements support it. That last step amends S4's "keep the final contributor as root" item. Numbers and steps are on #1823; they come from the #1935 A/B prototype on 4x L40S, not from S4.

