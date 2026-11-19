# MB_TRANSFER: transfer primitives on this machine (D2D / host bounce), grounding for the DF finish prototypes

2026-11-18. Standalone microbenchmark, raw CUDA + `rmm::mr::cuda_async_memory_resource` pools, **no engine code**.
Branch `mb/transfer` (worktree `$R/wt-mb-xfer`, off pr2570 `ae939824`), files `bench/transfer/{xfer_bench.cpp,xfer_kernels.cu,build.sh,tab.py}`.
Raw output `$R/runs/mb_xfer/out/*.log`, driver script `$R/runs/mb_xfer/bin/all.sh`. GiB = 2^30 bytes everywhere.

STATUS: main pass done and tabulated; contention follow-up (`cont2`) written, not run. See section 10.

## 0. Ownership of the transfer code (for any follow-up PR)

| Code | Where | Owner |
|---|---|---|
| Bloom finish: transport choice (all-or-nothing), HOST-staged loop, PEER `chunk_pipeline`, `or_bloom_chunk` | `src/cuda/sirius_dynamic_bloom_filter.cu` `finish_impl` ~:1130-1357, `:772-964` | **pre-existing (base-owned)**; #2569 only made the shapes per-key. A new schedule = its own PR outside the stack (or a split commit on top of #2569) |
| Replica copies (exact / small IN-list / ordinary Bloom): per target, whole D2H + sync, whole H2D + sync | `src/cuda/device_transfer.cpp:135-253` via `src/cuda/dynamic_filter_replica_transfer.cu:40-62` | **pre-existing** (last touched by #2302, before merge-base `c4fa3638`) |
| Pool peer access grant + `find_pair_without_pool_access` | `src/memory/pool_peer_access.cpp` | **pre-existing** |
| Chunk size clamp 2-8 MiB | `complete_build_inventory.hpp:259-263` | #2569 |
| Where the finish runs (Publishing Task, before deposit) | `dynamic_filter_publisher.cpp` `state::publish` | #2570 placement, #2569 session |

Engine facts the benchmark mirrors: device memory from a `cudaMemPool` (RMM async resource), peer access = legacy
`cudaDeviceEnablePeerAccess` + `cudaMemPoolSetAccess` per capable pair; host staging = 1 MiB blocks of a registered
(`mmap` + populate + `cudaHostRegister(Portable|Mapped)`) slab, copied through `cudaMemcpyBatchAsync`
(`specific_config.yaml`: `block_size: 1048576`); Bloom chunk = clamp(B/8, 2 MiB, 8 MiB).

## 1. Topology

- Instance `g7e.24xlarge` (KVM / Nitro guest). CPU: 1 socket Xeon Platinum 8559C, 48 cores / 96 threads, **one NUMA node**,
  1 TiB RAM, THP `madvise`. 4x RTX PRO 6000 Blackwell Server Edition, driver 595.112.07, CUDA runtime 13.5.
- PCIe (sysfs): every GPU and every bridge above it trains at **32 GT/s x16 (Gen5)**; all GPUs `numa_node=0`.
  - GPU0 `52:00.0`, GPU1 `53:00.0`: host bridge `pci0000:44` -> `44:00.0` -> `45:00.0` -> downstream ports `46:01.3` / `46:01.4`.
  - GPU2 `63:00.0`, GPU3 `64:00.0`: host bridge `pci0000:55` -> `55:00.0` -> `56:00.0` -> downstream ports `57:01.3` / `57:01.4`.
  - All bridges are `Amazon.com Device 0200` (the hypervisor's virtual switch hierarchy).
- `nvidia-smi topo -m`: 0-1 and 2-3 = `PIX` (at most one PCIe bridge); every cross pair = `NODE` (through PCIe host bridges
  inside one NUMA node). `nvidia-smi topo -p2p r` / `p`: `OK` inside the pairs, `NS` (not supported) across.
  Raw: `$R/runs/mb_xfer/topo.txt`.
- **Why the cross pairs are not P2P capable** (inference from the topology; no sudo, no dmesg): the two pairs sit under two
  different PCI host bridges. Peer DMA needs the request of one GPU to be routed to the other GPU's BAR below a common
  switch; routing between root ports / host bridges is optional in PCIe and is not offered by this virtualized platform
  (each pair is its own passthrough switch domain), so the driver reports no peer capability for those pairs. It is a
  property of the instance type, not a setting we can change (the guest exposes no IOMMU groups).
- NUMA placement of the pinned buffer (task item 2e): **not testable here, one NUMA node**. Host buffer kind is tested instead.

(Results sections follow.)

## 2. Link rates per GPU (task 2e, pinned vs pageable)

GiB/s, one leg alone, median over the 4 GPUs (min-max over GPUs). 5 reps after a warm-up; `*_sync` = a host sync after every chunk.

| leg | 8 MiB | 64 MiB | 256 MiB | 1434 MiB |
|---|---|---|---|---|
| d2h host=pinned dev=pool | 50.8 (50.3-51.0) | 52.7 (52.7-53.0) | 53.2 (53.1-53.2) | 53.2 (53.2-53.2) |
| h2d host=pinned dev=pool | 48.9 (48.5-49.1) | 51.3 (51.3-51.3) | 51.6 (51.6-51.6) | 51.7 (51.7-51.7) |
| d2h host=pinned dev=raw | 50.7 (50.5-50.8) | 52.9 (52.7-53.0) | 53.2 (53.1-53.2) | 53.3 (53.2-53.3) |
| h2d host=pinned dev=raw | 48.8 (48.6-48.9) | 51.3 (51.3-51.3) | 51.6 (51.6-51.6) | 51.7 (51.7-51.7) |
| d2h host=registered4k dev=pool | 50.7 (50.4-51.2) | 52.7 (52.7-53.0) | 53.2 (53.1-53.2) | 53.2 (53.2-53.2) |
| h2d host=registered4k dev=pool | 48.8 (48.6-48.8) | 51.3 (51.3-51.3) | 51.6 (51.6-51.6) | 51.7 (51.7-51.7) |
| d2h host=registeredthp dev=pool | 50.7 (50.7-51.3) | 52.9 (52.7-53.0) | 53.2 (53.2-53.2) | 53.3 (53.3-53.3) |
| h2d host=registeredthp dev=pool | 48.9 (48.8-48.9) | 51.4 (51.4-51.5) | 51.7 (51.7-51.7) | 51.8 (51.8-51.8) |
| d2h host=pageable dev=pool | 15.1 (15.1-15.2) | 16.5 (16.4-16.5) | 16.7 (16.6-16.7) | 16.7 (16.6-16.7) |
| h2d host=pageable dev=pool | 14.9 (14.9-15.0) | 16.0 (15.9-16.0) | 14.7 (13.7-15.0) | 9.9 (9.9-10.0) |
| d2h_sync chunk=8 blk=contig | 50.8 (50.5-51.0) | 50.6 (50.5-50.6) | 50.6 (50.6-50.7) | 50.6 (50.6-50.7) |
| h2d_sync chunk=8 blk=contig | 48.8 (48.7-48.8) | 48.9 (48.9-48.9) | 49.0 (48.9-49.0) | 49.0 (49.0-49.0) |
| d2h_sync chunk=8 blk=1m_batch | 49.7 (49.5-49.7) | 49.5 (49.5-49.7) | 49.6 (49.5-49.6) | 49.5 (49.5-49.6) |
| h2d_sync chunk=8 blk=1m_batch | 48.0 (47.7-48.3) | 47.8 (47.6-48.0) | 47.8 (47.8-47.9) | 47.9 (47.9-47.9) |
| d2h_sync chunk=2 blk=contig | 44.3 (44.3-44.4) | 44.0 (44.0-44.3) | 44.0 (44.0-44.0) | 44.1 (44.1-44.1) |
| h2d_sync chunk=2 blk=contig | 42.2 (42.1-42.3) | 42.2 (42.1-42.3) | 42.3 (42.3-42.5) | 42.4 (42.3-42.5) |

- D2H 53.2, H2D 51.7 GiB/s on every GPU, flat from 64 MiB up (about 90% of a Gen5 x16 link). GPU-to-GPU spread < 1%.
- Host buffer kind does not matter: `cudaHostAlloc`, registered 4K pages (the engine's recipe) and registered THP are equal.
  Pool (RMM async) and `cudaMalloc` device memory are equal.
- Pageable is 3.2x (D2H) to 3.5-5x (H2D) slower, and a pageable bounce is 8x slower than a pipelined pinned one (5.5-6 vs 50).
- A host sync per chunk costs 5% at 8 MiB and 17% at 2 MiB. The 1 MiB block batch (engine staging) costs 2% more.
  **Per-leg syncs are not what makes today's loop slow; the serial order of the legs is** (section 6).

## 3. Every ordered GPU pair (task 2a-2e)

Median over the ordered pairs of a class (4 P2P, 8 cross), 5 reps; destination bytes verified by a GPU kernel after the warm-up
and after the last rep (0 mismatches everywhere except the `!` rows). `stage_sync` = D2H then H2D, host sync after each leg
(today's engine shape). `stage_pipe` = D2H of chunk i+1 overlaps H2D of chunk i on separate streams, event-ordered, `slots`
host buffers, one final wait. `chunk=whole` = whole D2H then whole H2D (the replica-copy shape).


**P2P pairs** GiB/s, median over ordered pairs (min-max) [pairs]; '!' = wrong bytes or error

| method | 8 MiB | 64 MiB | 256 MiB | 1434 MiB |
|---|---|---|---|---|
| peerasync alloc=pool stream=dst | 49.3 (49.2-49.9) | 52.1 (52.0-52.2) | 51.0 (50.6-51.4) | 49.9 (49.7-50.0) |
| peerasync alloc=raw stream=dst | 49.8 (49.7-49.9) | 51.9 (51.3-52.3) | 50.6 (50.2-52.6) | 50.0 (49.7-50.0) |
| memcpyasync_d2d alloc=pool stream=dst | 49.5 (49.4-49.6) | 52.0 (52.0-52.0) | 52.3 (52.3-52.3) | 52.4 (52.4-52.4) |
| peerasync alloc=pool stream=src | 49.8 (49.8-49.8) | 52.4 (52.4-52.4) | 50.4 (50.4-50.4) | 49.8 (49.8-49.8) |
| stage_sync chunk=8 blk=1m_batch | 24.4 (24.3-24.4) | 24.3 (24.3-24.4) | 24.3 (24.3-24.3) | 24.3 (24.3-24.3) |
| stage_sync chunk=8 blk=contig | 24.8 (24.8-24.8) | 24.8 (24.8-24.9) | 24.8 (24.8-24.8) | 24.8 (24.8-24.8) |
| stage_sync chunk=2 blk=contig | 21.4 (21.4-21.5) | 21.5 (21.4-21.6) | 21.4 (21.4-21.4) | 21.4 (21.4-21.4) |
| stage_pipe chunk=8 slots=2 host=pinned | 25.2 (25.2-25.2) | 41.9 (41.8-41.9) | 44.9 (44.8-44.9) | 45.8 (45.8-45.8) |
| stage_pipe chunk=32 slots=2 host=pinned | 25.2 (25.2-25.2) | 33.0 (33.0-33.1) | 41.8 (41.8-41.8) | 45.1 (45.0-45.1) |
| stage_pipe chunk=whole slots=1 host=pinned | 25.2 (25.2-25.3) | 26.1 (26.1-26.2) | 26.2 (26.2-26.2) | 26.2 (26.2-26.2) |
| stage_pipe chunk=2 slots=2 host=pinned | 35.0 (35.0-35.0) | 40.0 (40.0-40.0) | 41.2 (41.2-41.2) | 41.3 (41.3-41.3) |
| stage_pipe chunk=16 slots=2 host=pinned | 25.3 (25.3-25.3) | 38.4 (38.4-38.4) | 44.0 (44.0-44.0) | 45.8 (45.8-45.8) |
| stage_pipe chunk=64 slots=2 host=pinned | 25.3 (25.3-25.3) | 26.0 (26.0-26.0) | 38.5 (38.5-38.5) | 44.3 (44.3-44.3) |
| stage_pipe chunk=8 slots=4 host=pinned | 25.3 (25.3-25.3) | 42.0 (42.0-42.0) | 45.0 (45.0-45.0) | 45.7 (45.7-45.7) |
| stage_pipe chunk=32 slots=4 host=pinned | 25.2 (25.2-25.2) | 33.1 (33.1-33.1) | 41.8 (41.8-41.8) | 45.0 (45.0-45.0) |
| stage_pipe chunk=32 slots=2 host=registered4k | 25.3 (25.3-25.3) | 33.0 (33.0-33.0) | 41.8 (41.8-41.8) | 45.1 (45.1-45.1) |
| stage_pipe chunk=32 slots=2 host=registeredthp | 25.3 (25.3-25.3) | 33.6 (33.6-33.6) | 43.1 (43.1-43.1) | 46.7 (46.7-46.7) |
| stage_pipe chunk=whole slots=1 host=registered4k | 25.3 (25.3-25.3) | 26.1 (26.1-26.1) | 26.2 (26.2-26.2) | 26.2 (26.2-26.2) |
| stage_sync chunk=whole host=pageable | 5.4 (5.4-5.4) | 5.8 (5.8-5.8) | 5.8 (5.8-5.8) | 6.0 (6.0-6.0) |
| stage_sync chunk=8 host=pageable | 5.5 (5.5-5.5) | 5.4 (5.4-5.4) | 5.5 (5.5-5.5) | 5.5 (5.5-5.5) |

**cross pairs** GiB/s, median over ordered pairs (min-max) [pairs]; '!' = wrong bytes or error

| method | 8 MiB | 64 MiB | 256 MiB | 1434 MiB |
|---|---|---|---|---|
| peerasync alloc=pool stream=dst | 33.0 (32.9-34.2) | 37.2 (37.0-37.3) | 37.4 (37.1-37.6) | 38.9 (38.8-39.0) |
| peerasync alloc=raw stream=dst | 33.5 (32.9-34.0) | 37.1 (36.9-37.3) | 37.3 (37.2-37.5) | 38.9 (38.8-39.0) |
| memcpyasync_d2d alloc=pool stream=dst | !8 | !8 | !8 | !8 |
| peerasync alloc=pool stream=src | 33.0 (32.8-33.7) | 37.0 (37.0-37.2) | 37.2 (37.2-37.2) | 39.0 (39.0-39.0) |
| stage_sync chunk=8 blk=1m_batch | 24.3 (24.3-24.5) | 24.4 (24.3-24.4) | 24.3 (24.3-24.4) | 24.3 (24.3-24.3) |
| stage_sync chunk=8 blk=contig | 24.8 (24.8-24.9) | 24.8 (24.8-24.9) | 24.8 (24.8-24.8) | 24.8 (24.8-24.8) |
| stage_sync chunk=2 blk=contig | 21.4 (21.4-21.6) | 21.5 (21.4-21.5) | 21.4 (21.4-21.4) | 21.4 (21.4-21.4) |
| stage_pipe chunk=8 slots=2 host=pinned | 25.2 (24.8-25.3) | 44.6 (44.3-44.7) | 48.5 (48.5-48.6) | 49.8 (49.7-49.8) |
| stage_pipe chunk=32 slots=2 host=pinned | 25.2 (24.8-25.3) | 34.5 (34.4-34.5) | 45.8 (45.8-45.8) | 50.3 (50.3-50.4) |
| stage_pipe chunk=whole slots=1 host=pinned | 25.2 (24.8-25.3) | 26.1 (26.1-26.1) | 26.2 (26.2-26.2) | 26.2 (26.2-26.2) |
| stage_pipe chunk=2 slots=2 host=pinned | 35.0 (34.8-35.2) | 42.6 (42.3-42.7) | 43.4 (43.3-43.4) | 44.0 (44.0-44.0) |
| stage_pipe chunk=16 slots=2 host=pinned | 25.2 (25.0-25.2) | 41.0 (41.0-41.0) | 48.2 (48.2-48.3) | 50.7 (50.7-50.7) |
| stage_pipe chunk=64 slots=2 host=pinned | 25.2 (24.9-25.3) | 26.1 (26.1-26.2) | 41.4 (41.4-41.4) | 49.4 (49.4-49.4) |
| stage_pipe chunk=8 slots=4 host=pinned | 25.2 (24.7-25.3) | 45.2 (45.0-45.3) | 49.4 (49.3-49.4) | 50.6 (50.6-50.7) |
| stage_pipe chunk=32 slots=4 host=pinned | 25.2 (24.9-25.2) | 34.5 (34.4-34.5) | 45.8 (45.8-45.8) | 50.4 (50.4-50.4) |
| stage_pipe chunk=32 slots=2 host=registered4k | 25.2 (24.9-25.3) | 34.5 (34.5-34.5) | 45.8 (45.8-45.8) | 50.4 (50.3-50.4) |
| stage_pipe chunk=32 slots=2 host=registeredthp | 25.2 (24.8-25.3) | 34.5 (34.5-34.6) | 45.9 (45.9-46.0) | 50.5 (50.5-50.5) |
| stage_pipe chunk=whole slots=1 host=registered4k | 25.2 (24.9-25.4) | 26.1 (26.0-26.2) | 26.2 (26.2-26.2) | 26.2 (26.2-26.2) |
| stage_sync chunk=whole host=pageable | 5.5 (5.5-5.6) | 5.8 (5.8-5.9) | 6.0 (5.8-6.0) | 6.0 (6.0-6.0) |
| stage_sync chunk=8 host=pageable | 5.5 (5.5-5.6) | 5.5 (5.4-5.6) | 5.5 (5.4-5.5) | 5.5 (5.5-5.5) |

Run without any peer access enabled (`nopeer=1`, separate process, all 12 ordered pairs): `cudaMemcpyPeerAsync` 34 / 37.5 / 37.6 /
39.5 GiB/s at 8 / 64 / 256 / 1434 MiB, bytes correct; plain `cudaMemcpyAsync(DeviceToDevice)` fails on all 12.
Probe run (`probe=1`): on the 8 cross pairs `cudaDeviceEnablePeerAccess` and `cudaMemPoolSetAccess` both return
`cudaErrorPeerAccessUnsupported`; `cudaMemcpyPeerAsync` still works afterwards (37.5 / 39.5 GiB/s).

- (a) P2P pairs, peer access on: **50-52 GiB/s (19.5-20 ms/GiB)**, same as a host leg; stream of source or destination, pool or raw: equal.
- (b) Cross pairs, `cudaMemcpyPeerAsync` without peer access: **works, bytes correct, 37-39 GiB/s** with pool allocations and no
  grant. The driver stages internally. Two limits make it a fallback, not the plan: it does not scale with concurrency
  (two cross copies together 43-45 GiB/s aggregate, section 5) and **the call blocks the submitting thread** while an earlier staged
  copy is still running (fan-out to 3 targets: 86 of 99 ms inside the calls; star of 8 MiB chunks: 109 of 130 ms).
  Plain `cudaMemcpyAsync` across non-peer devices is rejected (`cudaErrorInvalidValue`).
- (c) Today's shape (synced legs, 8 MiB chunks, 1 MiB blocks): **24.3 GiB/s (41 ms/GiB)** on every pair = half a link, because
  the two legs never overlap. 2 MiB chunks: 21.4.
- (d) Pipelined bounce: **49.8-50.7 GiB/s on cross pairs at 1.4 GiB = the P2P rate.** A host bounce costs no bandwidth here, only
  one chunk of latency. 8-16 MiB chunks with 2 slots are best (64 MiB arrays: 44.6 vs 34.5 at 32 MiB chunks); 4 slots add
  nothing; whole-array legs give 26.2 (no overlap). On a P2P pair the bounce reaches 45 (both legs share the pair's uplink).
- (e) Pageable: 5.5-6 GiB/s. NUMA: one node, not testable.

## 4. Fan-out of one finished array to the 3 other GPUs (task 2f; replica copies)

ms for the whole fan-out, payload on GPU 0, targets 1, 2, 3; bytes verified.

| config | ms | ms/GiB | submit ms |
|---|---|---|---|
| today_serial_per_target n=1 | 0.159 | 162.84 | -1.000 |
| staged_once n=1 chunk=whole | 0.080 | 82.39 | 0.010 |
| staged_once n=1 chunk=32 | 0.080 | 82.16 | 0.010 |
| p2p_partner+staged_once n=1 chunk=whole | 0.082 | 84.33 | 0.012 |
| p2p_partner+staged_once n=1 chunk=32 | 0.082 | 83.50 | 0.012 |
| p2p_partner+staged_one+p2p_relay n=1 chunk=32 | 0.080 | 82.00 | 0.014 |
| peerapi_all_targets n=1 chunk=whole | 0.099 | 101.59 | 0.020 |
| today_serial_per_target n=16 | 1.879 | 120.22 | -1.000 |
| staged_once n=16 chunk=whole | 0.922 | 59.04 | 0.011 |
| staged_once n=16 chunk=32 | 0.923 | 59.04 | 0.011 |
| p2p_partner+staged_once n=16 chunk=whole | 0.923 | 59.09 | 0.013 |
| p2p_partner+staged_once n=16 chunk=32 | 0.923 | 59.07 | 0.013 |
| p2p_partner+staged_one+p2p_relay n=16 chunk=32 | 0.923 | 59.07 | 0.015 |
| peerapi_all_targets n=16 chunk=whole | 1.162 | 74.35 | 0.114 |
| today_serial_per_target n=281 | 32.165 | 117.21 | -1.000 |
| staged_once n=281 chunk=whole | 15.765 | 57.45 | 0.011 |
| staged_once n=281 chunk=32 | 11.209 | 40.85 | 0.109 |
| p2p_partner+staged_once n=281 chunk=whole | 15.769 | 57.46 | 0.013 |
| p2p_partner+staged_once n=281 chunk=32 | 11.218 | 40.88 | 0.113 |
| p2p_partner+staged_one+p2p_relay n=281 chunk=32 | 10.562 | 38.49 | 0.124 |
| peerapi_all_targets n=281 chunk=whole | 20.257 | 73.82 | 6.710 |
| today_serial_per_target n=1434 | 163.832 | 116.99 | -1.000 |
| staged_once n=1434 chunk=whole | 80.331 | 57.36 | 0.012 |
| staged_once n=1434 chunk=32 | 54.661 | 39.03 | 0.573 |
| p2p_partner+staged_once n=1434 chunk=whole | 80.336 | 57.37 | 0.014 |
| p2p_partner+staged_once n=1434 chunk=32 | 54.676 | 39.04 | 0.561 |
| p2p_partner+staged_one+p2p_relay n=1434 chunk=32 | 53.613 | 38.28 | 0.613 |
| peerapi_all_targets n=1434 chunk=whole | 99.466 | 71.03 | 85.897 |

- Today (`device_transfer.cpp`: per target a whole D2H, sync, whole H2D, sync): 117 ms/GiB. Staging once and uploading to the
  three targets together: 57; pipelined in 32 MiB chunks: **39 ms/GiB (3x)**. The bound is the root's outgoing link
  (D2H once = 19 ms/GiB) plus the pair 2-3 uplink carrying two uploads (38 ms/GiB).
- P2P to the partner or a 2 -> 3 relay does not help further (38-39): P2P-out and D2H share the root's port and run one after
  the other, and the uplink of pair 2-3 is the limit either way.
- Small payloads (exact-key replicas, 1-16 MiB): 0.16 -> 0.08 ms at 1 MiB, 1.9 -> 0.9 ms at 16 MiB. Not a suite item.

## 5. Concurrency (task 3)

1434 MiB per leg, all legs submitted together, per-leg completion read by polling events. `D0` = D2H from GPU 0, `H2` = H2D to
GPU 2, `P01` = `cudaMemcpyPeerAsync` 0 -> 1, `B02` = pipelined bounce 0 -> 2 (32 MiB chunks). 256 MiB legs give the same rates.

| config | med_ms | agg_gibps | leg_gibps |
|---|---|---|---|
| d2h:0 | 26.296 | 53.26 | D0=53.3 |
| d2h:0,1 | 52.587 | 53.26 | D0=26.6,D1=26.6 |
| d2h:0,2 | 26.348 | 106.30 | D0=53.2,D2=53.2 |
| d2h:0,1,2 | 52.650 | 79.79 | D0=26.6,D1=26.6,D2=53.2 |
| d2h:1,2,3 | 52.610 | 79.86 | D1=53.2,D2=26.6,D3=26.6 |
| d2h:0,1,2,3 | 52.689 | 106.31 | D0=26.6,D1=26.6,D2=26.6,D3=26.6 |
| h2d_onebuf:0 | 27.029 | 51.81 | H0=51.8 |
| h2d_onebuf:0,1 | 53.973 | 51.89 | H0=25.9,H1=25.9 |
| h2d_onebuf:0,2 | 27.035 | 103.60 | H0=51.8,H2=51.8 |
| h2d_onebuf:1,2,3 | 53.979 | 77.83 | H1=51.8,H2=25.9,H3=25.9 |
| h2d_onebuf:0,1,2,3 | 53.977 | 103.78 | H0=25.9,H1=25.9,H2=25.9,H3=25.9 |
| h2d_sepbuf:1,2,3 | 53.975 | 77.84 | H1=51.8,H2=25.9,H3=25.9 |
| h2d_sepbuf:0,1,2,3 | 54.014 | 103.71 | H0=25.9,H1=25.9,H2=25.9,H3=25.9 |
| duplex:d2h0+h2d0 | 29.678 | 94.37 | D0=47.3,H0=47.2 |
| duplex:d2h0+h2d2 | 27.090 | 103.39 | D0=53.3,H2=51.7 |
| duplex:d2h0,1+h2d2,3 | 54.133 | 103.48 | D0=26.6,D1=26.6,H2=25.9,H3=25.9 |
| duplex:d2h0-3+h2d0-3 | 68.425 | 163.73 | D0=20.5,D1=20.5,D2=20.5,D3=20.5,H0=20.8,H1=20.8,H2=20.8,H3=20.8 |
| p2p:01 | 28.134 | 49.78 | P01=49.8 |
| p2p:23 | 28.121 | 49.80 | P23=49.8 |
| p2p:01+23 | 28.088 | 99.71 | P01=49.9,P23=49.9 |
| p2p:01+10 | 28.939 | 96.78 | P01=48.9,P10=48.5 |
| p2p:01+10+23+32 | 28.948 | 193.50 | P01=48.6,P10=48.5,P23=48.5,P32=48.4 |
| p2p:01x2_same_dir | 56.194 | 49.84 | P01=49.8,P01=24.9 |
| p2p:01+d2h0 | 54.290 | 51.59 | P01=50.0,D0=25.8 |
| p2p:01+h2d1 | 53.314 | 52.53 | P01=26.3,H1=36.7 |
| p2p:01+h2d0 | 34.063 | 82.22 | P01=41.4,H0=41.1 |
| p2p:01+d2h1 | 28.168 | 99.43 | P01=49.7,D1=52.2 |
| p2p:01+d2h2+h2d3 | 29.566 | 142.10 | P01=50.0,D2=48.2,H3=47.4 |
| peerapi_cross:02 | 35.818 | 39.10 | P02=39.1 |
| peerapi_cross:02+13 | 65.330 | 42.87 | P02=27.0,P13=21.4 |
| peerapi_cross:02+20 | 61.866 | 45.27 | P02=29.0,P20=22.6 |
| peerapi_cross:02+p2p01 | 63.857 | 43.86 | P02=21.9,P01=27.8 |
| peerapi_cross:02+12 | 71.400 | 39.23 | P02=24.2,P12=19.6 |
| bounce:02 | 27.812 | 50.35 | B02=50.4 |
| bounce:02+13 | 55.064 | 50.86 | B02=25.5,B13=25.4 |
| bounce:02+20 | 34.077 | 82.19 | B02=41.3,B20=41.1 |
| bounce:02+p2p01 | 55.688 | 50.29 | B02=25.1,P01=48.1 |
| bounce:02+13+20+31 | 70.161 | 79.84 | B02=20.1,B13=20.1,B20=20.0,B31=20.0 |
| bounce:02+03 | 54.760 | 51.15 | B02=25.9,B03=25.6 |
| bounce:02+12 | 55.415 | 50.54 | B02=25.3,B12=25.8 |

- **The shared resource is the uplink of each GPU pair, not host memory.** D2H from GPUs 0 and 1 together: 53 GiB/s total (26.6 each);
  from 0 and 2: 106. All four: 106 = two uplinks. H2D the same (52 per pair; one source buffer or separate buffers: equal).
  Host memory keeps up with 164 GiB/s (4 D2H + 4 H2D at once).
- Duplex is nearly free: D2H and H2D on one GPU run at 47 + 47; all eight host legs at once give 41 up + 41 down per uplink.
- P2P stays inside the switch: 0 -> 1 and 2 -> 3 together 100 GiB/s, both directions of both pairs 194 GiB/s, and a P2P copy
  into GPU 1 does not slow a D2H from GPU 1 (49.7 + 52.2).
- A GPU's own port is used by one outgoing copy at a time: P2P 0 -> 1 with D2H from 0 = 50 then 26 (queued, not shared);
  incoming P2P and H2D on the same GPU share (26 + 37).
- Driver-staged cross copies do not add up: two at once 43-45 GiB/s aggregate; the manual bounce in both directions 82.

## 6. The DF shape end to end (task 4)

G = 4, every GPU holds a partial array of B bytes, all four must end with the OR of all four (S = 3 sources into root 0, T = 3
targets for the star). ms = median of 5 [min-max]; (ms per GiB of B). All four results verified against the expected OR after
the warm-up and the last rep: 0 mismatches in every row. OR kernel = the engine's shape (16-byte loads, 256 threads).

| schedule | B=57 MiB: ms [min-max] (ms/GiB) | B=281 MiB: ms [min-max] (ms/GiB) | B=1434 MiB: ms [min-max] (ms/GiB) |
|---|---|---|---|
| today blk=1m_batch | 10.5 [10.5-10.5] (188) | 51.1 [51.1-51.1] (186) | 260.1 [260.0-260.3] (186) |
| hyper r1=p2p r2=bounce chunk=32 | 4.8 [4.8-4.8] (87) | 21.1 [20.2-21.3] (77) | 94.7 [93.7-95.0] (68) |
| c2 r2=bounce chunk=32 | 3.3 [3.3-3.3] (59) | 14.4 [14.3-14.7] (52) | 72.2 [72.1-72.4] (52) |
| hostor_pipe chunk=32 threads=16 | 4.4 [4.2-4.7] (79) | 18.8 [18.6-19.0] (68) | 106.7 [95.6-106.8] (76) |
| star_peerapi chunk=8 | 5.6 [5.6-5.6] (100) | 25.9 [25.7-25.9] (94) | 130.1 [129.9-130.2] (93) |
| today blk=contig | 10.3 [10.2-10.3] (184) | 50.2 [50.2-50.3] (183) | 255.9 [255.8-256.0] (183) |
| today blk=contig chunk=64 | 9.7 [9.7-9.7] (174) | 47.7 [47.7-47.8] (174) | 243.1 [243.1-243.2] (174) |
| star_peerapi chunk=32 | 6.8 [6.7-6.8] (121) | 26.0 [25.9-26.0] (95) | 124.9 [124.5-125.0] (89) |
| star_pipe chunk=8 | 5.0 [5.0-5.0] (90) | 22.7 [22.6-22.7] (83) | 114.7 [114.5-126.3] (82) |
| star_pipe chunk=32 | 6.3 [6.2-6.3] (112) | 24.9 [24.9-25.0] (91) | 125.0 [124.5-125.0] (89) |
| c1 fan=relay chunk=32 | 4.4 [4.3-4.4] (78) | 16.4 [16.4-16.5] (60) | 77.0 [76.7-77.3] (55) |
| c1 fan=host3 chunk=32 | 4.7 [4.7-4.7] (85) | 16.3 [16.1-16.5] (59) | 80.7 [80.6-80.7] (58) |
| c2 r2=bounce chunk=8 | 3.1 [3.0-3.1] (55) | 14.2 [14.1-14.5] (52) | 73.8 [72.2-73.8] (53) |
| c2 r2=bounce chunk=64 | 4.5 [4.5-4.5] (81) | 15.3 [15.2-15.3] (56) | 73.4 [72.9-73.6] (52) |
| c2 r2=peerapi chunk=32 | 3.9 [3.9-4.0] (71) | 17.1 [17.1-17.2] (62) | 87.1 [86.9-87.2] (62) |
| hyper r1=p2p r2=bounce chunk=8 | 4.2 [4.2-4.2] (76) | 18.9 [18.8-19.2] (69) | 94.2 [93.9-95.1] (67) |
| hyper r1=p2p r2=bounce chunk=64 | 5.6 [5.6-5.6] (101) | 21.1 [21.1-21.1] (77) | 95.3 [95.0-97.1] (68) |
| hyper r1=p2p r2=peerapi chunk=32 | 4.3 [4.3-4.5] (77) | 20.7 [20.5-21.0] (75) | 105.8 [105.2-106.4] (76) |
| hyper r1=bounce r2=bounce chunk=32 | 6.3 [6.3-6.4] (114) | 25.5 [25.5-25.6] (93) | 136.6 [135.7-138.0] (98) |
| hostor_3phase threads=16 | 5.7 [5.7-5.8] (103) | 28.3 [28.2-28.5] (103) | 146.6 [146.5-146.7] (105) |
| hostor_pipe chunk=8 threads=16 | 3.0 [3.0-3.0] (53) | 17.3 [17.1-17.3] (63) | 97.9 [97.8-98.0] (70) |
| hostor_pipe chunk=64 threads=16 | 5.7 [5.5-5.7] (102) | 19.3 [19.3-19.4] (70) | 96.6 [96.6-97.2] (69) |
| hostor_pipe chunk=32 threads=4 | 4.6 [4.6-4.6] (83) | 24.7 [24.7-24.8] (90) | 136.0 [135.9-136.1] (97) |
| hostor_pipe chunk=32 threads=8 | 4.1 [4.1-4.1] (74) | 19.3 [19.3-19.4] (70) | 104.1 [104.0-104.4] (74) |
| hostor_pipe chunk=32 threads=32 | 3.9 [3.8-3.9] (70) | 18.2 [18.2-18.3] (66) | 96.3 [96.3-96.4] (69) |
| pairhostor_pipe chunk=32 threads=16 | 3.8 [3.8-3.9] (69) | 14.7 [14.5-15.2] (53) | 72.6 [72.5-72.7] (52) |
| pair01_star_peer chunk=8 | 1.4 [1.4-1.5] (26) | 6.3 [6.3-6.4] (23) | 31.4 [31.4-31.4] (22) |
| pair01_star_peer chunk=32 | 1.8 [1.8-1.8] (32) | 6.4 [6.4-6.4] (23) | 30.0 [29.9-30.1] (21) |
| pair01_exchange chunk=8 | 1.2 [1.2-1.3] (22) | 5.9 [5.9-6.0] (22) | 30.2 [30.2-30.3] (22) |
| pair01_exchange chunk=32 | 1.2 [1.2-1.2] (22) | 6.0 [5.9-6.0] (22) | 30.1 [30.1-30.2] (22) |
| pair01_exchange_bounce chunk=32 | 3.5 [3.5-3.5] (63) | 12.8 [12.8-12.8] (47) | 60.8 [60.8-60.8] (43) |

Schedules (code: `sched_*` in `xfer_bench.cpp`):

| name | what it does |
|---|---|
| `today` | the engine's HOST_STAGED loop: star through root 0, per chunk (clamp(B/8, 2, 8 MiB)) each source D2H + sync, H2D to root + sync, OR; root D2H + sync; 3 uploads + 3 syncs; one staging window of 1 MiB blocks |
| `star_peerapi` | the engine's PEER pipeline run on all 4 GPUs (what dropping the all-or-nothing rule gives): `cudaMemcpyPeerAsync` on every leg, cross legs staged by the driver |
| `star_pipe` | same star, explicit pipelined host bounce on every leg, no P2P, OR on the root GPU |
| `c1` | the proposed schedule (i): P2P merge 1 -> 0 and 3 -> 2, one bounce 2 -> 0, then 0 -> 1 by P2P, 0 -> host once -> 2, and 2 -> 3 by P2P (`relay`) or host -> 3 (`host3`) |
| `c2` | P2P merge 1 -> 0 and 3 -> 2, **symmetric** exchange 0 <-> 2 through the host (both directions at once), OR on 0 and 2, P2P fan-out 0 -> 1 and 2 -> 3 |
| `hyper` | no root: round 1 every GPU exchanges with its pair partner, round 2 with the GPU of the other pair (`r1` / `r2` = P2P, bounce, or driver-staged peer API) |
| `hostor_3phase` / `hostor_pipe` | schedule (ii): all 4 D2H in parallel, OR of the 4 arrays on the CPU (AVX-512, non-temporal stores), 4 H2D in parallel; three synced phases, or pipelined per chunk |
| `pairhostor_pipe` | P2P merge 1 -> 0 and 3 -> 2 on the GPUs, D2H of the two pair results, CPU OR of 2, H2D to all 4 |
| `pair01_*` | G = 2 on the P2P pair only: the engine's PEER star, and a duplex exchange |

Reading:

- **`today` reproduces the engine: 186 ms/GiB** (engine: 181 in MB4, 193 on the suite). It moves 10 x B through 7 serial legs + 3
  parallel uploads per chunk, each leg at one link rate: 7 x 19.5 + ~25 + OR = the measured 186. Larger chunks or contiguous staging: 174-183.
- **Best here: `c2` at 52 ms/GiB (3.6x), flat from 281 MiB up; 55-59 at 57 MiB** (8 MiB chunks: 55). `pairhostor_pipe` ties (52-53)
  at the large sizes. `c1` (the proposal as written) 55-60: its fan-out is a second serial bounce; the symmetric exchange of `c2`
  uses both directions of both uplinks at once instead.
- `hyper` 68-77 and `hostor_pipe` 53-79: they push 2 x B up and 2 x B down each pair uplink (all four GPUs talk to the host), where
  `c2` pushes B each way. `hostor_pipe` needs >= 16 CPU threads (4 threads: 90-97).
- `star_peerapi` (delete the all-or-nothing rule, keep the engine's pipeline): 93-100 = **2x for almost no code**, but its submit
  blocks (109 of 130 ms) and see section 7.
- No P2P at all (forced): `hostor_pipe` 53-76, `star_pipe` 82-90, `hyper` all-bounce 93-114, `hostor_3phase` 103-105.
- P2P pair alone (G = 2): 21-23 ms/GiB for the engine's PEER star (MB4: 20-22.5) and the same for a duplex exchange.
- Submit cost of the event-chained schedules: 2.4 ms (`c2`, 32 MiB chunks, 1.4 GiB) to 4.3 ms (`hyper`); with 8 MiB chunks about 4x that.

Bound from the measured link rates (per GiB of B; port = 50-53 GiB/s one way, 47 each way duplex; pair uplink 53 up / 52 down,
41 each way with all eight legs):

| schedule | busiest resource | bound ms/GiB | measured | measured / bound |
|---|---|---|---|---|
| `today` | 7 serial legs + 1 parallel stage | ~160 + OR | 186 | 1.15 |
| `c2` | port of GPU 0 / GPU 2: receives 2 x B (P2P + host), sends 2 x B | 39-43 | 52 | 1.2-1.3 |
| `pairhostor_pipe` | uplink down 2 x B; port of GPU 0 receives 2 x B | 39 | 52 | 1.3 |
| `hostor_pipe` | uplink 2 x B up and 2 x B down (duplex) | 43-49 | 53-76 | 1.2-1.6 |
| `hyper` | port 2 x B each way; uplink 2 x B each way | 43-49 | 68 | 1.4-1.6 |
| any schedule on this topology | each pair uplink carries >= B up and >= B down; some GPU receives >= 2 x B unless the host ORs | ~39 | - | - |

## 7. Contention with an unrelated heavy cross-GPU copy (task 3, models the build CONCAT move)

Background thread(s), back to back, whole 1434 MiB legs: `p2p01` = `cudaMemcpyPeerAsync` 0 -> 1; `host02` = whole D2H from GPU 0,
then whole H2D to GPU 2 (cuCascade's host-staged shape). Foreground as in section 6 (ms, ms/GiB), 5 reps.

| schedule | B=57 MiB: ms (ms/GiB) | B=281 MiB: ms (ms/GiB) | B=1434 MiB: ms (ms/GiB) |
|---|---|---|---|
| today blk=1m_batch bg=none | 10.5 (188) | 51.1 (186) | 260.2 (186) |
| hyper r1=p2p r2=bounce chunk=32 bg=none | 4.8 (87) | 21.0 (77) | 98.9 (71) |
| c2 r2=bounce chunk=32 bg=none | 3.3 (59) | 15.0 (54) | 75.0 (54) |
| hostor_pipe chunk=32 threads=16 bg=none | 4.6 (83) | 18.8 (69) | 96.1 (69) |
| star_peerapi chunk=8 bg=none | 5.6 (100) | 25.8 (94) | 130.2 (93) |
| today blk=1m_batch bg=p2p01 | 230.1 (4133) | 1033.7 (3767) | 5168.3 (3691) |
| hyper r1=p2p r2=bounce chunk=32 bg=p2p01 | 60.9 (1095) | 103.9 (379) | 687.5 (491) |
| c2 r2=bounce chunk=32 bg=p2p01 | 31.3 (563) | 40.9 (149) | 90.6 (65) |
| hostor_pipe chunk=32 threads=16 bg=p2p01 | 31.9 (572) | 43.1 (157) | 109.5 (78) |
| star_peerapi chunk=8 bg=p2p01 | 1070.6 (19234) | 5097.5 (18576) | 33533.6 (23946) |
| today blk=1m_batch bg=host02 | 429.4 (7713) | 1932.4 (7042) | 9662.9 (6900) |
| hyper r1=p2p r2=bounce chunk=32 bg=host02 | 56.8 (1020) | 275.6 (1004) | 1116.9 (798) |
| c2 r2=bounce chunk=32 bg=host02 | 81.0 (1456) | 296.0 (1079) | 1264.6 (903) |
| hostor_pipe chunk=32 threads=16 bg=host02 | 57.0 (1024) | 72.2 (263) | 153.6 (110) |
| star_peerapi chunk=8 bg=host02 | 1548.4 (27818) | 7528.6 (27435) | 38282.6 (27337) |
| today blk=1m_batch bg=p2p01+host02 | 436.5 (7842) | 1968.7 (7174) | 9876.4 (7053) |
| hyper r1=p2p r2=bounce chunk=32 bg=p2p01+host02 | 166.7 (2994) | 352.3 (1284) | 1386.5 (990) |
| c2 r2=bounce chunk=32 bg=p2p01+host02 | 83.4 (1498) | 300.9 (1096) | 1269.3 (906) |
| hostor_pipe chunk=32 threads=16 bg=p2p01+host02 | 56.9 (1022) | 71.9 (262) | 153.1 (109) |
| star_peerapi chunk=8 bg=p2p01+host02 | 2269.9 (40778) | 11218.4 (40881) | [LOST: not in transcript] |
| config | med_ms | agg_gibps | leg_gibps |
|---|---|---|---|
| p2p:01 n=1434 bg=none | 28.075 | 49.88 | P01=49.9 |
| p2p:23 n=1434 bg=none | 28.096 | 49.84 | P23=49.8 |
| bounce:02 n=1434 bg=none | 27.782 | 50.41 | B02=50.4 |
| bounce:13 n=1434 bg=none | 27.694 | 50.57 | B13=50.6 |
| p2p:01 n=1434 bg=p2p01 | 56.290 | 24.88 | P01=24.9 |
| p2p:23 n=1434 bg=p2p01 | 28.037 | 49.95 | P23=49.9 |
| bounce:02 n=1434 bg=p2p01 | 673.399 | 2.08 | B02=2.1 |
| bounce:13 n=1434 bg=p2p01 | 27.919 | 50.16 | B13=50.2 |
| p2p:01 n=1434 bg=host02 | 54.202 | 25.84 | P01=25.8 |
| p2p:23 n=1434 bg=host02 | 28.463 | 49.20 | P23=49.2 |
| bounce:02 n=1434 bg=host02 | 1228.249 | 1.14 | B02=1.1 |
| bounce:13 n=1434 bg=host02 | 53.567 | 26.14 | B13=26.1 |
| p2p:01 n=1434 bg=p2p01+host02 | [LOST: not in transcript] |
| p2p:23 n=1434 bg=p2p01+host02 | [LOST: not in transcript] |
| bounce:02 n=1434 bg=p2p01+host02 | [LOST: not in transcript] |
| bounce:13 n=1434 bg=p2p01+host02 | [LOST: not in transcript] |
- background throughput while the foreground ran: bg=none, bg_p2p_gibps=0.00, bg_host_gibps=0.00
- background throughput while the foreground ran: bg=p2p01, bg_p2p_gibps=49.25, bg_host_gibps=0.00
- background throughput while the foreground ran: bg=host02, bg_p2p_gibps=0.00, bg_host_gibps=26.14
- background throughput while the foreground ran: bg=p2p01+host02, [LOST: not in transcript]

- **A GPU runs queued copies in arrival order and does not interleave them.** A foreground leg that becomes runnable only after
  a previous leg (a host sync, or an event from another GPU) queues behind a whole background copy every time. Cost = number of
  gated legs on the contended GPU x duration of one background leg (28-55 ms here).
- `today`: 186 -> **3700-7000 ms/GiB (20-37x)**: 180 chunks x 7 gated legs. q21's 1.39 GiB would take 5-10 s.
  `star_peerapi` 8 MiB chunks: 19000-40000 ms/GiB. The background copy itself keeps its full rate (49 and 26 GiB/s).
- `c2` with 32 MiB chunks: 65-150 with the P2P background, **900-1100** with the host-staged one (45 gated chunks x 28 ms).
- `hostor_pipe` is the least sensitive: 78-157 (P2P background), **110-263** (host-staged): its downloads are all queued up front
  and its uploads are queued in bursts by the host thread.
- Consequence for the eager-scan design (finish runs while the build CONCAT move is in flight): **few, large legs, queued up
  front, beat many small gated legs.** Inferred from the rows above, **not yet measured**: the follow-up that runs `c2` /
  `hostor` with 256 MiB and whole-array legs, and a background of 64 MiB legs, is written (`tests=cont2`, `bgleg=`) but did
  not get the machine lock (section 10).
- Caveat: the background is a worst case (saturated, 1.4 GiB legs, same GPUs). A real CONCAT move has gaps and smaller buffers.

## 8. nsys view (B = 281 MiB, `--trace=cuda,nvtx`, capture `$R/runs/mb_xfer/nsys/df281.{nsys-rep,sqlite}`)

| schedule (B = 281 MiB, last rep, under nsys) | wall ms | copy operations | GiB copied | host waits (cudaStreamSynchronize + cudaEventSynchronize) |
|---|---|---|---|---|
| today blk=1m_batch | 52.1 | 360 | 2.74 | 388 |
| star_peerapi chunk=8 | 27.2 | 360 | 2.74 | 28 |
| star_pipe chunk=32 | 24.2 | 90 | 2.74 | 28 |
| c2 r2=bounce chunk=32 | 14.7 | 72 | 2.20 | 28 |
| c1 fan=relay chunk=32 | 16.5 | 72 | 2.20 | 28 |
| hyper r1=p2p r2=bounce chunk=32 | 20.9 | 108 | 3.29 | 28 |
| hostor_pipe chunk=32 threads=16 | 20.2 | 72 | 2.20 | 40 |
| pairhostor_pipe chunk=32 threads=16 | 14.7 | 72 | 2.20 | 46 |
| pair01_star_peer chunk=8 | 6.4 | 72 | 0.55 | 28 |

`today` issues 360 copies and 388 host waits for 2.74 GiB (10 x B); `c2` 72 copies, 28 waits (all of them the final joins),
2.19 GiB (8 x B: 4 P2P legs + 2 bounces). The per-kind split CUPTI reports (HtoD / DtoH / PtoP) did not match the calls issued
for the host legs (e.g. 216 "PtoP" in `today`, which issues none); not resolved, so only totals are shown.

## 9. Recommended schedule per machine class, expected ms/GiB and suite-level finish

Suite model: 3188 MiB of Bloom arrays per pass (20 keys; 1117 MiB in arrays <= 133 MiB, 733 MiB at 174-281 MiB, 1394 MiB for q21),
each size class priced with the measured ms/GiB at 57 / 281 / 1434 MiB, S = 3 assumed for every publication. Check: `today`
prices to 591 ms; the suite measured 600 ms. Uncontended numbers; nothing below was run in the engine.

| Machine | Schedule | ms/GiB (57 / 281 / 1434 MiB) | Suite finish (today 600 ms) | Status |
|---|---|---|---|---|
| (2) partial P2P, this one | today | 188 / 186 / 186 | 591 (measured 600) | measured |
| | **`c2`: P2P merge inside each pair, symmetric pipelined bounce between the pair roots, P2P fan-out** | 55-59 / 52 / 52 | **~170** (-430 ms, 2.8% of the suite; q21 296 -> ~71, q3 56 -> ~18) | measured (microbench) |
| | `pairhostor_pipe` (P2P merge, CPU OR of the two pair results, 4 uploads) | 69 / 53 / 52 | ~185 | measured |
| | `hostor_pipe` (no P2P use, CPU OR of 4, >= 16 threads, 8 MiB chunks) | 53 / 63 / 70 | ~200 | measured |
| | `star_peerapi` (engine PEER pipeline on every pair, driver stages the cross legs) | 100 / 94 / 93 | ~300 | measured; submit blocks |
| (3) no P2P | `hostor_pipe` (8 MiB chunks) | 53 / 63 / 70 | ~200 | measured here with P2P unused; this box still has 2 GPUs per uplink |
| | pipelined star through the root (`star_pipe`, 8 MiB) | 90 / 83 / 82 | ~270 | measured, P2P unused |
| | with one uplink per GPU (not this box): `hostor_pipe` bound = B up + B down per link, duplex | ~21-24 | ~70 | **model only** |
| (1) full P2P | engine PEER star as is: root receives 3 x B and sends 3 x B | 62-67 | ~195-210 | model from the pair (22 ms/GiB at G = 2; MB4 20-22.5 per target) |
| | `c2` shape with a P2P cross edge (or `hyper`): 2 x B per port instead of 3 x B | ~44 (port bound 41) | ~135 | **model only**: two rounds at the pair's measured 22 ms/GiB |

What to build, by class:

1. **Partial P2P (here):** choose the transport per edge, not per publication. Merge inside each P2P group over P2P, exchange
   the group results between the group roots through a pipelined host bounce (8-32 MiB chunks, 2 slots, separate streams per
   flow, events, one final wait), OR on the GPUs, fan out inside each group over P2P. One bounce costs the same as a P2P leg.
2. **Full P2P:** the same code with the cross edge on P2P. The win over today's PEER star is the topology (2 x B per port
   instead of 3 x B), not the transport; at G = 8 a star is 7 x B per root port, a tree / hypercube 3 x B.
3. **No P2P:** OR on the host. All downloads in parallel into pinned memory, AVX-512 OR with >= 16 threads (4 inputs: 36 GiB/s
   at 16 threads, 39 at 32; 2 inputs: 58 / 66), all uploads in parallel. It uses every link once in each direction.
4. **Replica copies (exact / small IN-list):** stage once, upload to all targets together (117 -> 39-57 ms/GiB). Small payloads,
   not a suite item.
5. **Do not rely on the driver's staging** (`cudaMemcpyPeerAsync` on non-peer pairs): correct and usable from pool memory
   without any grant, 39 GiB/s alone, but it blocks the caller and does not scale. Good as a zero-code fallback only.
6. Pinned host memory (already the case), never pageable. NUMA: nothing to do on a one-node box.

Ownership: items 1-3 change `finish_impl` / `chunk_pipeline` (pre-existing, base-owned) and item 4 `device_transfer.cpp`
(pre-existing): **each needs its own PR outside #2569 / #2570**. The stack only fixes where the finish runs (#2570) and the
chunk clamp (#2569, `complete_build_inventory.hpp:259-263`).

## 10. What ran, variance, what is unverified

- One locked pass (`flock machine.lock runs/mb_xfer/bin/all.sh`, lock held 23:13-23:37 UTC, 24 min, most of it the contention
  block): build, smoke (all tests, 1 rep, 0 mismatches), main (`out/main.log`, 1196 result lines, 5 reps + warm-up each,
  exit 0), no-peer run (`out/nopeer.log`), probe run (`out/probe.log`, 3 reps), nsys capture of the DF block at 281 MiB (2 reps).
  Each configuration ran in **one process, once**; no repeat of the whole pass on another day / after a reboot.
- Variance: min-max of the 5 reps is within 1% for almost every row (tables show it). Outliers: `hostor_pipe` 32 MiB chunks at
  1434 MiB 95-107 ms; `star_pipe` 8 MiB at 1434 MiB 114-126 ms.
- Correctness: every pair / fan-out / DF row checks the destination bytes on the GPU (0 mismatches). The 32 error rows are all
  plain `cudaMemcpyAsync(DeviceToDevice)` across non-peer devices.
- Device memory came from `rmm::mr::cuda_async_memory_resource` pools with the engine's grants; host staging from
  `cudaHostAlloc` or a registered slab. cuCascade's host pool itself was not linked (same recipe, emulated).
- **Unverified / not done:** no engine prototype, no TPC-H timing (the suite numbers in section 9 are a model on top of microbench
  rates); full-P2P 4-GPU and one-uplink-per-GPU rows are models; NUMA placement not testable; the CUPTI copy-kind split is
  unexplained; why cross pairs lack P2P is inferred from the topology, not confirmed with the platform; S < 3 shapes not run;
  contention used a saturated synthetic background, not a real CONCAT move.
- **Queued, never ran:** `runs/mb_xfer/bin/cont2.sh` (leg-size robustness under background copies) waited 74 min for the
  machine lock behind ~130 other waiters and was abandoned. The committed `xfer_bench.cpp` contains that patch (`cont2`,
  `bgleg`, `run_df` mode 2, buffers of 2 x BMAX): **it was never compiled or run**. The binary in `runs/mb_xfer/bin` and
  every number in this file come from the source before that patch.
- No Quent telemetry was produced (no engine process). Nothing written to the root disk. `$R/repo` only read (its pixi
  toolchain and headers).

Reproduce:
```bash
R=/opt/dlami/nvme/dfperf
flock $R/machine.lock $R/runs/mb_xfer/bin/all.sh          # resumable: delete out/.ok_<phase> to rerun a phase
flock $R/machine.lock $R/runs/mb_xfer/bin/cont2.sh        # follow-up, not run yet -> out/cont2.log; tab.py out/cont2.log cont
python3 $R/wt-mb-xfer/bench/transfer/tab.py $R/runs/mb_xfer/out/main.log df    # leg | pair | fan | hostor | conc | df | cont
$R/runs/mb_xfer/bin/xfer_bench tests=df dfsizes=281 reps=5                     # keys: tests sizes dfsizes fansizes reps bmax nopeer probe bgleg
```
