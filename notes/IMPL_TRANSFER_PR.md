# IMPL_TRANSFER_PR: production implementation of the transfer PR [xfer-PR]

Branch `prod/xfer` (from `pr2570` ae939824), worktree `$R/wt-prod-xfer`. Design: DESIGN_TRANSFER_PR.md (amended by step 0).
Build: `$R/tmp/xfer_build.sh` (flock + pixi run make, log `$R/logs/build_prod_xfer.log`).

(sections filled at the end; see checkpoint log at the bottom for current state)

## Checkpoint log (chronological; a fresh session resumes from the last line)
- 11-28 start: worktree created (`git -c protocol.file.allow=always submodule update --init --recursive`), baseline build started.
- commits 1-3 done (17333840 peer_islands, ff23950c host_or + workers, e01a623a union_plan), tests green ([peer_islands],[host_or],[union_plan]).
- WIP (uncommitted in worktree): src/cuda/union_transfer.cu + detail/union_transfer.hpp (unite: merge / cross with CPU OR / fan-out,
  one drain + one release), finish_impl switched to unite, chunk_pipeline + tip HOST loop + force_host + publication_transport deleted,
  publication_route + degraded, counter accumulation_publications_degraded, manager holds host_or_workers::shared().
  Tests: test seam = builder_access::finish(builder, root, stream, mr, {.islands, .inline_or, .report}); acc::separate_gpus.
  Fault test file rewritten (not yet built). TODO: fix remaining test call sites (test_dynamic_filter_accumulation.cpp ~2078-2230,
  test_in_task_publication.cpp 852-960, test_pool_peer_access.cpp, multi_partition integration), new test file
  test_dynamic_filter_union_transfer.cpp (identity over topologies, degradation, concurrency, inserts in flight), docs, gates.
- helper scripts: $R/tmp/xfer_build.sh, xfer_test.sh <bin> <catch args>, xfer_pc.sh (pre-commit), xfer_commit.sh "<msg>".
