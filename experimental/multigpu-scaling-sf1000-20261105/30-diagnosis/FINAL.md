# Sirius 1→4 GPU scaling — ranked diagnosis (verified)

TPC-H SF1000, tables pinned in HOST memory (lineitem+orders Simpatico-compressed), warm, 4x RTX PRO 6000, commit bb557b1a.
Figure of merit: hot-mean suite total. g1 35.82 s → g4 20.04 s (1.79x). Ideal g4 8.96 s. **Gap 11.08 s.**
Sources: RANKED.md (synthesis waterfall, 61 findings → 11 root causes), verify_*.md (2 adversarial verifiers per top-8 item),
10-analysis/{X1_allocator,X2_exchange,X3_link,fam_*}.md, 20-experiments/, 10-analysis/microbench/.

## Ranked bottlenecks (wall s on g4 suite; "verified" = midpoint of the two verifier corrections)

| # | Bottleneck | Kind | Synth | Verified | Key evidence |
|---|---|---|---|---|---|
| 1 | Host-link ceiling: 2 Gen5 x16 uplinks feed 4 GPUs (GPU0+1 and GPU2+3 share a switch) → 52→105 GB/s, a 2x link for 4x GPUs | hardware | 3.48 | **2.9** | microbench 52/52/102/105 GB/s; g2 same-switch 1.22x vs cross-switch 1.61x; gpupin −3.8 s on 19 clean queries; Q17/19/20 at link floor |
| 2 | Process-wide CUDA driver stream-ordered allocator lock (cudaMallocFromPoolAsync/cudaFreeAsync straight from cucascade adaptor → rmm cuda_async, no caching layer) | mechanical | 1.80 | **2.0** (1.5–2.4) | microbench: aggregate ~280–385k calls/s regardless of GPU count; mean 10.9 µs (1 GPU×4 thr) → 56 µs (4×4); nsys 7.3→42.4 s malloc, 2.7→35.7 s free; 81% of alloc time in driver locks at g4; thr8 +1.19 s; predicts batch4g gains r=0.96 |
| 3 | Per-fragment cross-GPU clone overhead: 627 scan tasks × P fragments, each cloned serially in the consumer's prepare, per-buffer syncs, single fan-in task per partition | design | 1.72 | **1.2** | clone thread-time 0.23 s (g1) → 25.4 s (g4); Q18 MERGE prepare 27 ms → 6.1 s thread; Q1 "straggler" = one MERGE task cloning 470 partials serially (148 ms); hpb256 23.66 s; batch4g removes ~2.1 s of exchange excess |
| 4 | Cross-switch shuffle via serialized 2-leg host staging (cross-switch P2P unsupported by platform; D2H→sync→H2D→sync per 1 MiB chunk) | hardware + design | 1.23 | **0.9** (≈0.5 hw floor + 0.4 staging) | 117.6 GB staged (66.6%, all cross-switch) vs 59 GB peer; staged clone 3.6x peer cost/byte; 6.4 host syncs/staged clone |
| 5 | Pipeline serialization: next scan created only after previous exchange barrier; link idle during exchange phases (multi-GPU prefetch/lookahead off) | design | 1.20 | **0.7** | per-switch link idle 10.25 s (g1) → 8.05 s (g4) only; 4.6 s of ≥5 ms interior gaps; task_scheduler.cpp:209-221, creator/config.hpp:101,106 |
| 6 | Exchange policy: P≥N forced partitioning (exchanges pass-through at N=1 hash-partition at N=4), BUILD_PROBE min(nat,N), shuffle where broadcast would do (max_broadcast_join_size 256 MB, max_build_hash_table_bytes 2 GB) | design + config | 0.76 | **0.7** | forced-class PARTITION 0.13 s (g1) → 16.0 s thread (g4); Q4 PARTITION 9→897 ms; joinpol g4 −0.97 s (oracle −1.4), g1 −1.08 s (Q3 join-mode confound) |
| 7 | Fixed serial per-query work outside execution (parse/bind/optimize, result return) — Amdahl | design | 0.34 | **0.34** | ~442 ms/suite outside Quent window, identical g1/g4 |
| 8 | Granularity tails: P mod N biased to GPU0/1, scan-task quantization, single-GPU tails | config/design | 0.31 | **0.24** | Quent occupancy loss; small queries ~0.5–0.85 occupancy |
| 9 | Accumulated-Bloom publication all-or-nothing HOST staging (any non-P2P pair ⇒ whole publication staged) | design | 0.30 | 0.05–0.3 (not verified) | 60/60 publications staged at g4 vs peer at {0,1}; 20 ms mean latency |
| 10 | Single small-pinned host pool for all GPUs (1 NUMA node): device_scalar syncs wait on another GPU's pooled ready_event | design | 0.20 | 0.2 (not verified) | 105k cross-device pooled-event syncs, 13.1 thread-s at g4, 0 at g1 |
| 11 | Measurement credits (g1 Q13 not warm; Q3 join-mode confound) | measurement | −0.25 | −0.25 | g1 Q13 iters 5.03/3.92/2.47/2.65 |
| | Sum | | 11.08 | ≈9.1 | **≈2 s of the gap not attributed after verifier trims** (bucket uncertainty ±0.8 s) |

Grouped: hardware topology ≈3.4 s (#1 + hw part of #4); cross-GPU exchange design ≈2.8 s (#3, #4 staging, #6, #9); driver allocator ≈2.0 s; pipeline serialization ≈0.7 s; Amdahl/tails/misc ≈0.8 s.

## Measured config levers (un-profiled, hot mean, s)

| config | g1 | g4 | scaling |
|---|---|---|---|
| baseline (specific_config + capacity 600GB) | 35.82 | 20.04 | 1.79x |
| scan_task_batch_size 4GB | 33.04 | **16.91** | **1.95x** |
| scan_task_batch_size 8GB | – | 17.34 | – |
| batch4g + joinpol (default build cap 8GB, broadcast 8GB) | 31.93 | 16.92 | 1.89x |
| joinpol only | 34.74 | 19.07 | 1.82x |
| hash_partition_bytes 256MB | – | 23.66 | – |
| hash_partition_bytes 4GB | – | 19.91 | – |
| pipeline.num_threads 8 | 35.55 | 21.23 | 1.67x |
| pin tier=gpu (no host link; memory-pressure confounded) | – | 18.26 | – |
| 2 GPUs {0,1} same switch / {0,2} cross switch | 29.40 / 22.21 | | 1.22x / 1.61x |
Quent on/off: 20.04 / 20.00 (no telemetry effect). Noise: g4 suite sd ≈0.02–0.04 s.

## Remedies (by expected g4 gain)
1. Config now: scan_task_batch_size 4GB (−3.1 s g4); keep 4 threads/GPU; for 2 GPUs use gpu_ids [0,2] not num_gpus: 2.
2. User-space sub-allocator between cucascade reservation adaptor and cuda_async (per-GPU arena / thread-local bins ≤1–4 MiB) via reservation_manager_configurator set_gpu_memory_resource_factory: ≈2.3–3.7 s g4 (+0.9–2.1 s g1). Also avoid P×C deep-copy slices in gpu_partition_impl.cpp:212-221.
3. Exchange redesign: coalesce fragments per (producer GPU, partition) before cloning; batch/overlap clone legs (async, event-ordered, not per-buffer sync); cost-aware broadcast; drop P≥N floor where a local/broadcast plan exists: ≈2–3 s.
4. Cross-pipeline scan lookahead/prefetch to fill link-idle exchange phases: ≈0.7–1 s.
5. Bytes: enable compression plans for customer/part/partsupp/supplier, better o_comment / l_shipmode encodings (−233 GB, 19% of wire bytes): ≈1.1 s g4.
6. Hierarchical Bloom publication (peer within switch islands, stage only inter-island); per-device small-pinned pools.
Best achievable on this hardware at today's wire bytes ≈12.2–12.4 s (≈2.9x); 4x needs fewer bytes on the uplinks or different topology.
