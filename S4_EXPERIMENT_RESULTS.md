# S4 four-GPU screening results

## Conclusion

**Retain control (`current_direct`, including bounded HOST fallback).** The complete four-L40S/SF100/default256 screen passed correctness and provenance checks. Concurrent ingress increased median paired builder publication time by **48.0–54.9%** on the 64 MiB/root sweep, near-256 MiB case, and two-key case. SQL Q7/Q21 publication intervals increased **47.2%/46.7%** for equivalent admitted and published work. The small one-key case improved only 3.3%; forced HOST was effectively unchanged (+0.03%).

This is a screening observation, **not a four-GPU topology winner or a final production selection**. Each comparison has one independent process pair with three measured repetitions, so there is no selection confidence interval. End-to-end query ratios also increased, but baseline controls drifted by 2.1–4.7%; these observations cannot establish precise query regressions or satisfy the held-out 3% regression guard.

Recommendation: do not advance the unchanged concurrent-ingress pilot to held-out confirmation on the strength of this screen. Keep control plus required HOST fallback while the coordinator reviews this evidence. If further diagnosis is useful, run a separate bounded profiler comparison at one-key 64 MiB+256 on roots 0 and 3, and Q21, to inspect ingress/OR/egress overlap and contention. The cause of the slowdown is unmeasured; no source fix, new topology, confirmation, all22 run, or pruning was performed.

## Reproduction

- Source commit: `9b2e8201787f381b62715c01405f659b925c49a7`, checked out unchanged through all measurements.
- Prerequisite tree: `9f59d68e4cdc7ca47999c3579407f6f5168ea652`; `git cat-file -t` returned `tree`. It is the tree of merge `78a3e34c66143152ff075729f4c018806c11e67d`.
- Isolated source/build root: `/tmp/s4-four-gpu-20261023`. The original `/home/ubuntu/sirius` checkout and its unrelated files/submodule checkouts were preserved. Tracked-source status was clean before building and after measurements; evidence/report files are the only intended additions.
- Evidence run ID: `20261023T150247Z-l40s-screen`; native campaign ID: `20261023T150247Z-l40s-screen-1792771793028254847`.
- `S4_EVIDENCE_DIR=/tmp/s4-four-gpu-20261023/docs/experiments/s4-results/20261023T150247Z-l40s-screen` in the campaign wrapper.
- Screen UTC: **2026-10-23T16:09:52.129804+00:00** to **2026-10-23T16:17:57.696996+00:00** (485.567 s). Machine snapshots: `2026-10-23T16:09:53Z` to `2026-10-23T16:17:56Z`. Preparation/build/test command timestamps are in each `*.command.json`.
- Seed 1823; four explicit GPU IDs `0,1,2,3`; default256 cap 268,435,456 bytes/GPU, coverage 0.9; GPU pinning, compression disabled; three warmups and three measured repetitions per process. Randomized matched arm order and baseline/baseline controls came from the frozen runner manifest.
- Dataset: `/data/tpch_parquet_sf100`, 14 Parquet files. Scale 100 is supported by generator metadata and independently summed Parquet footer counts, not its directory name. Content hashes of the complete dataset were not measured. File paths/sizes/mtimes and footer counts are in [dataset-identity.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/dataset-identity.json); generator metadata is in [dataset-generator-metadata.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/dataset-generator-metadata.json).

| Table | Independently counted rows |
| --- | --- |
| customer | 15000000 |
| lineitem | 600037902 |
| nation | 25 |
| orders | 150000000 |
| part | 20000000 |
| partsupp | 80000000 |
| region | 5 |
| supplier | 1000000 |

Required build-submodule initialization completed successfully:

```bash
git submodule update --init --recursive -- \
  duckdb cucascade substrait .claude/claude-tools \
  sirius-duckdb/vcpkg sirius-duckdb/duckdb sirius-duckdb/extension-ci-tools
```

Pinned revisions used (the complete recursive status, including intentionally uninitialized unrelated experimental submodules, is in [dependency-revisions.log](docs/experiments/s4-results/20261023T150247Z-l40s-screen/dependency-revisions.log)):

| Dependency | Revision |
| --- | --- |
| duckdb and sirius-duckdb/duckdb | 88ecd0a49d7d3d838e002b2fbc0ce91ccf74110e |
| cucascade | 1a9281093a0fe49634594d541d7ce327129f53d6 |
| substrait | 922489d3c8c77140b189ad3847554de141378b04 |
| .claude/claude-tools | bcfccda6e35b50f356df99c2baae16204901ede3 |
| sirius-duckdb/vcpkg | cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3 |
| sirius-duckdb/extension-ci-tools | 55e9f697b4fb70f357cc1bd4d1893cbf217ee359 |
| substrait/duckdb | d8cdaa33fda8df955cc76ef58a280f68f4cd43fa |
| substrait/extension-ci-tools | 72e76e99cd7fee45a99739cd118ec2db64e034ec |
| substrait/substrait | 29844f1b58a666f36493c27def553854e81328a7 |

Exact successful build, test, screen, and analyzer invocations follow. All commands ran from the isolated source root; the shared Pixi environment was explicitly activated through `pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml`.

```bash
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/build_s4_experiments.py --output build/s4-retry-xxhash --cuda-architectures native --jobs 24 --cmake-option=-DXXHSUM_EXECUTABLE=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/bin/xxhsum --cmake-option=-DSIMPATICO_XXHASH_INCLUDE_DIR=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/include
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/s4-retry-xxhash/current_direct/sirius_unittest '[s4_publication]' --order decl
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/s4-retry-xxhash/current_direct/sirius_cuda_fault_test
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/s4-retry-xxhash/concurrent_ingress/sirius_unittest '[s4_publication]' --order decl
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/s4-retry-xxhash/concurrent_ingress/sirius_cuda_fault_test
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python -m unittest discover -s scripts -p test_s4_experiments.py -v
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/run_s4_experiments.py --help
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/analyze_s4_experiments.py --help
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/run_s4_experiments.py screen --stages transport,builder,sql --gpus 0,1,2,3 --build-manifest build/s4-retry-xxhash/build-manifest.json --data-path /data/tpch_parquet_sf100 --scale 100 --profile default256 --queries 4,21,7 --warmups 3 --pairs 3 --seed 1823 --output build/s4-results/20261023T150247Z-l40s-screen
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/analyze_s4_experiments.py --results build/s4-results/20261023T150247Z-l40s-screen --output /tmp/s4-four-gpu-20261023/docs/experiments/s4-results/20261023T150247Z-l40s-screen/screen-analysis.json
```

Environment deviation: the existing activated environment has Python 3.12.14 although the manifest requests Python 3.14. It has CUDA 13.4.92, GCC 15.3.0, cuDF 26.08.01 and RMM 26.08.00. It was reused unchanged rather than re-resolved. Both schedules used the same dependencies. The first configuration failed because `xxhsum` was absent from this environment; a first retry located the cached executable but also needed `xxhash.h`. Both failed helper attempts returned 1 and are preserved. The successful retry used explicit cached xxhash 0.8.3 executable/header paths in both configurations and a new output directory, `build/s4-retry-xxhash`. No measured source change was made. This explains the deviation from the guide's initial `build/s4` artifact path.

Installed packages and supplemental tool hashes: [installed-packages.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/installed-packages.json), [build-environment.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/build-environment.json). Both build manifests, commands and caches: [build-records/build-manifest.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/build-records/build-manifest.json). Compiler architecture was detected using `native` on the L40S host. The paired source SHA256 is `34813aae30bdc5d11502adc8adce467dbea9fc3392aa1e7ef185ad9206585aa2`; normalized configuration SHA256 is `cd014835cba3a83e90d35510998378f77c4ac54855f9aac39a86738c60b72eac` in both builds.

| Artifact SHA256 | current_direct | concurrent_ingress |
| --- | --- | --- |
| fixture | 85840cb714f0435ae0c7a56b8fd7ff06a85ff093168986c8f4eaf66273df37f9 | fa380fc5b8ea32cadccda121f35d8c84bb0ec8df8701af2fb673e98642d4dd6b |
| library | 465c0a7b020e66d9d9ee53b5958b620438f182792394060009827304641343c7 | d2c0a1e1cabe539764d638361ec8d82ea93ddef325c3607d712e1b28fd88f36a |
| extension | a9704c497b555998b2024a1eebcdb6a02067418fb94abb92d876fd9934d70028 | c6c84753333ccf616d2b2fe25862c0b32da6e65e0d3a86b3398595e0cc97c1a7 |
| cli | 8982c9ef8389ad14bbefbe4610eaa37356425a512dc1791d7d703a45a4a65e81 | 8982c9ef8389ad14bbefbe4610eaa37356425a512dc1791d7d703a45a4a65e81 |

## Machine

CUDA visibility mapping was unset, so requested ordinals 0–3 are the driver ordinals below. All GPUs are NVIDIA L40S with 46,068 MiB visible memory and 350 W power caps; persistence mode was enabled. Driver/KMD is 615.71.09; toolkit is CUDA 13.4.92. GPU clocks were not locked or modified. Preflight clocks were 210 MHz SM / 405 MHz memory (P8); full before/after samples include observed clocks and power states.

| Ordinal | UUID | Model | PCI bus | Memory | Screen-start SM/memory clocks | P-state |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | GPU-874bc7df-e2cc-9ff8-6c5b-6c7972cf7cf0 | NVIDIA L40S | 00000002:00:01.0 | 46068 MiB | 210 MHz/405 MHz | P0 |
| 1 | GPU-20d23e0a-42ef-3ed4-a307-d190b4ed0c05 | NVIDIA L40S | 00000002:00:02.0 | 46068 MiB | 210 MHz/405 MHz | P0 |
| 2 | GPU-1775fd41-525a-5ff6-e62d-ac61b1651418 | NVIDIA L40S | 00000002:00:03.0 | 46068 MiB | 210 MHz/405 MHz | P0 |
| 3 | GPU-9014ca3c-c056-2956-06ce-72492b72ab80 | NVIDIA L40S | 00000002:00:04.0 | 46068 MiB | 210 MHz/405 MHz | P0 |

All off-diagonal topology entries are PHB, with CPU affinity 0–31 / NUMA 0. Driver peer-read and peer-write matrices report OK for all 12 ordered pairs. Those matrices establish driver capability only; Stage A supplies byte-checked evidence from actual owning pools. Physical transfer routes remain unmeasured.

The KVM guest reports 32 logical CPUs, AMD EPYC 9254 24-Core Processor, one NUMA node; process CPU affinity and default memory policy span CPUs 0–31 / node 0. The preflight and campaign-boundary GPU snapshots showed no competing GPU process. Experiment processes were sequential and the campaign owned the observed GPU work; competing host load was not continuously traced. No external idle-window reservation or continuous clock/power telemetry was available. The sandbox itself cannot see GPU devices; native builds/tests/screen ran outside it.

Runner-generated configuration selected GPUs 0–3, GPU capacity fraction 0.4, HOST fraction 0.5, two initial 128 MiB HOST pools and 1 MiB HOST blocks. The same frozen configuration was used by both builds' SQL and loader processes. Native transport/builder modes use their fixture-owned real pools, as described by their requests. The native fixture identifies static `sirius_core` linkage; the external extension/CLI load check is a distinct loading path.

Nsight Systems 2024.5.1 was discoverable but no profiler trace was collected. Physical-route counters, local CUDA stage-event durations, actual pinned-memory NUMA placement, per-device allocator peak/delta and allocator committed/cache peaks were not measured. Workspace charges below are not total process memory or allocator committed peaks. Full machine observations are archived as `machine-before.json`, `machine-after.json`, and returned separately in the preflight snapshot.

## Correctness

| Build/check | Cases/tests | Assertions | Exit | Skips |
| --- | --- | --- | --- | --- |
| current_direct focused [s4_publication] | 5 | 1687 | 0 | none reported |
| current_direct isolated fault | 7 | 832 | 0 | none reported |
| concurrent_ingress focused [s4_publication] | 5 | 1687 | 0 | none reported |
| concurrent_ingress isolated fault | 7 | 832 | 0 | none reported |
| Python harness unittest suite | 20 | unittest assertion count not emitted | 0 | none |
| runner/analyzer --help | 2 commands | not applicable | 0 | none |
| paired build + identity verification | both frozen schedules | not applicable | 0 | none |
| screen / analyzer / independent raw verification | 86 processes / 516 records | native oracles, not Catch assertion counts | 0 | none |

Focused checks covered canonical union/replicas and workspace accounting, local-only staging, HOST exhaustion with unchanged partials, scratch reservation accounting, and publication-byte counting. Fault tests exercised their existing isolated injected-error/delay paths. These are fresh host results, not the historical 94-case suites. No new sanitizer run, all22 regression or full unit-suite pass is claimed.

The complete campaign contains 258 warmups and 258 measured records: 252 transport, 192 builder and 72 SQL records including warmups. All 516 have `status=ok` and passed oracles. There were no unavailable/skipped workloads, cap/resource refusals, failed campaign processes or failed GPU-interception/profile checks. Transport checked every destination byte and immutable sources. Builder checked the CPU OR of all pre-publication partial bytes against every replica and checked inserted-key membership with no false negatives; all builder records report complete byte accounting. SQL used exact full, type/null-aware CPU result comparison outside GPU timing, rather than a digest-only oracle.

All 72 SQL records report verified default profile, GPU interception, one successful execution/rebind, and zero planner/runtime fallbacks or read-view mismatches. Q4/Q21/Q7 returned 5/100/4 rows respectively with CPU parity. Q7/Q21 work counters are identical between schedules and matched controls; Q4 has no accumulated publication in either arm.

The helper verified distinct fixture/library hashes, compiled schedules, equal source/configuration, CLI version, and extension/library resolutions. Both CLI smoke processes used `-bail -no-agent`, reached `s4_extension_loaded`, and their loader diagnostics identify their own frozen library. Successful SQL output alone was not used as interception proof. The independent check also verified completion hashes and equality of all aggregate records to untouched native JSONL after removing harness metadata. No mismatch was found.

Per-command logs/counts are in [current_direct-focused.log](docs/experiments/s4-results/20261023T150247Z-l40s-screen/current_direct-focused.log), [current_direct-fault.log](docs/experiments/s4-results/20261023T150247Z-l40s-screen/current_direct-fault.log), [concurrent_ingress-focused.log](docs/experiments/s4-results/20261023T150247Z-l40s-screen/concurrent_ingress-focused.log), [concurrent_ingress-fault.log](docs/experiments/s4-results/20261023T150247Z-l40s-screen/concurrent_ingress-fault.log), [harness-tests.log](docs/experiments/s4-results/20261023T150247Z-l40s-screen/harness-tests.log) and [independent-bundle-verification.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/independent-bundle-verification.json). A loader warning about missing version information in the installed `libcusolver.so.12` was present in the test logs; all tests and library-resolution checks passed.

## Stage A

All 42 scheduled workloads completed, each with three warmups and three measured samples. All 12 ordered pairs, all-pairs concurrency, and inbound/outbound concurrency at each of four roots were tested in both issuing directions using real owning-pool allocations and grants. Each edge carried 67,109,120 logical bytes. Pair/root/all-pairs totals were respectively 67,109,120 / 201,327,360 / 805,309,440 bytes per repetition. Copies reported `cudaMemcpyPeerAsync`; issuer/stream ownership was recorded, without a physical-route claim. No direct access was unavailable.

Across measured samples, isolated pairs delivered 26.126–26.239 logical GB/s; root patterns delivered 26.277–26.361 GB/s. Median concurrent all-pairs rates were 36.721 GB/s for destination reads and 38.603 GB/s for source writes. Root patterns were near 26.3 GB/s in both directions. There is no material root-specific or directional disadvantage visible in these uninstrumented diagnostics. Aggregate all-pairs throughput did not approach four times one pair's throughput; this is shared-capacity evidence, not identification of a physical bottleneck.

The table includes every workload and all measured host intervals. Edge totals are counted once. Overlapping device-event times were not collected or added together.

| Workload | Pattern / ordered edges | Root | Issuer | Logical bytes / repetition | Host elapsed ns (3 repetitions) | Median logical GB/s |
| --- | --- | ---: | --- | ---: | --- | ---: |
| transport-00 | pair: 0→1 | 1 | destination_read | [67109120] | 2562353, 2560973, 2561743 | 26.197 |
| transport-01 | pair: 0→2 | 2 | destination_read | [67109120] | 2563963, 2564013, 2568652 | 26.173 |
| transport-02 | pair: 0→3 | 3 | destination_read | [67109120] | 2564423, 2565223, 2563563 | 26.169 |
| transport-03 | pair: 1→0 | 0 | destination_read | [67109120] | 2564733, 2565924, 2564994 | 26.163 |
| transport-04 | pair: 1→2 | 2 | destination_read | [67109120] | 2562483, 2566173, 2563173 | 26.182 |
| transport-05 | pair: 1→3 | 3 | destination_read | [67109120] | 2564283, 2564344, 2564133 | 26.171 |
| transport-06 | pair: 2→0 | 0 | destination_read | [67109120] | 2560403, 2562903, 2560683 | 26.208 |
| transport-07 | pair: 2→1 | 1 | destination_read | [67109120] | 2566082, 2565003, 2566063 | 26.153 |
| transport-08 | pair: 2→3 | 3 | destination_read | [67109120] | 2560003, 2561633, 2560113 | 26.213 |
| transport-09 | pair: 3→0 | 0 | destination_read | [67109120] | 2563673, 2562893, 2563952 | 26.177 |
| transport-10 | pair: 3→1 | 1 | destination_read | [67109120] | 2564973, 2563453, 2565123 | 26.164 |
| transport-11 | pair: 3→2 | 2 | destination_read | [67109120] | 2564733, 2564213, 2564392 | 26.170 |
| transport-12 | all_pairs: 0→1; 0→2; 0→3; 1→0; 1→2; 1→3; 2→0; 2→1; 2→3; 3→0; 3→1; 3→2 | 0 | destination_read | [805309440] | 21930753, 22173376, 21882213 | 36.721 |
| transport-13 | root_inbound: 1→0; 2→0; 3→0 | 0 | destination_read | [201327360] | 7638788, 7639029, 7639179 | 26.355 |
| transport-14 | root_outbound: 0→1; 0→2; 0→3 | 0 | destination_read | [201327360] | 7658049, 7659529, 7657279 | 26.290 |
| transport-15 | root_inbound: 0→1; 2→1; 3→1 | 1 | destination_read | [201327360] | 7640718, 7638599, 7638288 | 26.357 |
| transport-16 | root_outbound: 1→0; 1→2; 1→3 | 1 | destination_read | [201327360] | 7659079, 7656399, 7657708 | 26.291 |
| transport-17 | root_inbound: 0→2; 1→2; 3→2 | 2 | destination_read | [201327360] | 7642549, 7640888, 7639419 | 26.349 |
| transport-18 | root_outbound: 2→0; 2→1; 2→3 | 2 | destination_read | [201327360] | 7659119, 7658139, 7658658 | 26.288 |
| transport-19 | root_inbound: 0→3; 1→3; 2→3 | 3 | destination_read | [201327360] | 7644789, 7642199, 7643279 | 26.340 |
| transport-20 | root_outbound: 3→0; 3→1; 3→2 | 3 | destination_read | [201327360] | 7658279, 7656989, 7661819 | 26.289 |
| transport-21 | pair: 0→1 | 1 | source_write | [67109120] | 2561524, 2561943, 2560783 | 26.199 |
| transport-22 | pair: 0→2 | 2 | source_write | [67109120] | 2561003, 2560633, 2560794 | 26.206 |
| transport-23 | pair: 0→3 | 3 | source_write | [67109120] | 2568483, 2558803, 2557803 | 26.227 |
| transport-24 | pair: 1→0 | 0 | source_write | [67109120] | 2558843, 2558892, 2560113 | 26.226 |
| transport-25 | pair: 1→2 | 2 | source_write | [67109120] | 2563313, 2561643, 2562973 | 26.184 |
| transport-26 | pair: 1→3 | 3 | source_write | [67109120] | 2559953, 2562093, 2561203 | 26.202 |
| transport-27 | pair: 2→0 | 0 | source_write | [67109120] | 2563303, 2559673, 2559223 | 26.218 |
| transport-28 | pair: 2→1 | 1 | source_write | [67109120] | 2563213, 2563323, 2562513 | 26.182 |
| transport-29 | pair: 2→3 | 3 | source_write | [67109120] | 2560613, 2558813, 2558403 | 26.227 |
| transport-30 | pair: 3→0 | 0 | source_write | [67109120] | 2562423, 2562693, 2562553 | 26.188 |
| transport-31 | pair: 3→1 | 1 | source_write | [67109120] | 2558263, 2557653, 2558033 | 26.235 |
| transport-32 | pair: 3→2 | 2 | source_write | [67109120] | 2561242, 2559692, 2562643 | 26.202 |
| transport-33 | all_pairs: 0→1; 0→2; 0→3; 1→0; 1→2; 1→3; 2→0; 2→1; 2→3; 3→0; 3→1; 3→2 | 0 | source_write | [805309440] | 20859089, 20861759, 20861320 | 38.603 |
| transport-34 | root_inbound: 1→0; 2→0; 3→0 | 0 | source_write | [201327360] | 7639888, 7638799, 7639938 | 26.352 |
| transport-35 | root_outbound: 0→1; 0→2; 0→3 | 0 | source_write | [201327360] | 7647989, 7648079, 7648029 | 26.324 |
| transport-36 | root_inbound: 0→1; 2→1; 3→1 | 1 | source_write | [201327360] | 7637468, 7637299, 7638669 | 26.360 |
| transport-37 | root_outbound: 1→0; 1→2; 1→3 | 1 | source_write | [201327360] | 7649169, 7649368, 7649239 | 26.320 |
| transport-38 | root_inbound: 0→2; 1→2; 3→2 | 2 | source_write | [201327360] | 7643239, 7641089, 7642969 | 26.342 |
| transport-39 | root_outbound: 2→0; 2→1; 2→3 | 2 | source_write | [201327360] | 7650718, 7651228, 7648129 | 26.315 |
| transport-40 | root_inbound: 0→3; 1→3; 2→3 | 3 | source_write | [201327360] | 7642149, 7641259, 7642359 | 26.344 |
| transport-41 | root_outbound: 3→0; 3→1; 3→2 | 3 | source_write | [201327360] | 7650939, 7650348, 7650669 | 26.315 |

## Builder

All byte columns are logical quantities or checked-out workspace charges as labeled. Each contributor inserts 4,096 rows/key; four contributors and four replicas are used. Builder returns ready filters and has zero channel-published bytes. Timing is host publication preparation through completion.

| Workload | Root | Contributors | K | B/key bytes | C bytes | Transport | Input bytes | Ready bytes | Logical edges bytes | Scratch requested/aligned bytes | Physical HOST bytes | D2H/H2D bytes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| builder-k1-b1048832-r3 | 3 | 4 | 1 | 1048832 | 1048832 | "direct" | 4195328 | 4195328 | 6292992 | 3146496/3146496 | 0 | 0/0 |
| builder-k1-b67109120-r0 | 0 | 4 | 1 | 67109120 | 8388608 | "direct" | 268436480 | 268436480 | 402654720 | 50331648/50331648 | 0 | 0/0 |
| builder-k1-b67109120-r1 | 1 | 4 | 1 | 67109120 | 8388608 | "direct" | 268436480 | 268436480 | 402654720 | 50331648/50331648 | 0 | 0/0 |
| builder-k1-b67109120-r2 | 2 | 4 | 1 | 67109120 | 8388608 | "direct" | 268436480 | 268436480 | 402654720 | 50331648/50331648 | 0 | 0/0 |
| builder-k1-b67109120-r3 | 3 | 4 | 1 | 67109120 | 8388608 | "direct" | 268436480 | 268436480 | 402654720 | 50331648/50331648 | 0 | 0/0 |
| builder-k1-b268435200-r3 | 3 | 4 | 1 | 268435200 | 8388608 | "direct" | 1073740800 | 1073740800 | 1610611200 | 50331648/50331648 | 0 | 0/0 |
| builder-k2-b67109120-r3 | 3 | 4 | 2 | 67109120 | 8388608 | "direct" | 536872960 | 536872960 | 805309440 | 50331648/50331648 | 0 | 0/0 |
| builder-force-host | 3 | 4 | 2 | 67109120 | 8388608 | "host" | 536872960 | 536872960 | 805309440 | 8388608/8388608 | 8388608 | 536872960/805309440 |

| Workload | Control median ms | Candidate median ms | Control p90 ms | Candidate p90 ms | C/B median paired ratio | C/B p90 ratio | B/B median ratio | B/B p90 ratio |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| builder-k1-b1048832-r3 | 0.321534 | 0.310484 | 0.323006 | 0.310812 | 0.9669 | 0.9722 | 0.9911 | 0.9969 |
| builder-k1-b67109120-r0 | 9.095187 | 13.723227 | 9.199925 | 13.751204 | 1.5110 | 1.5124 | 1.0308 | 1.0374 |
| builder-k1-b67109120-r1 | 9.078633 | 13.706254 | 9.101113 | 13.716326 | 1.5098 | 1.5109 | 1.0031 | 1.0040 |
| builder-k1-b67109120-r2 | 9.272523 | 13.732513 | 9.279267 | 13.877571 | 1.4814 | 1.4956 | 0.9974 | 0.9992 |
| builder-k1-b67109120-r3 | 9.257322 | 13.692111 | 9.260586 | 13.847776 | 1.4804 | 1.4961 | 1.0034 | 1.0039 |
| builder-k1-b268435200-r3 | 33.991980 | 52.665763 | 34.019035 | 52.677410 | 1.5493 | 1.5547 | 1.0008 | 1.0015 |
| builder-k2-b67109120-r3 | 17.559761 | 26.713798 | 17.570202 | 26.721758 | 1.5213 | 1.5224 | 0.9992 | 1.0006 |
| builder-force-host | 52.781887 | 52.784075 | 52.805056 | 52.833492 | 1.0003 | 1.0006 | 0.9997 | 1.0001 |

All raw measured host intervals, in ns (warmups remain in the archive):

| Workload | Comparison | Baseline ns | Other arm ns |
| --- | --- | --- | --- |
| builder-k1-b1048832-r3 | baseline_candidate | 323374, 318915, 321534 | 309434, 310484, 310894 |
| builder-k1-b1048832-r3 | baseline_baseline | 319864, 322174, 311334 | 317014, 311014, 310804 |
| builder-k1-b67109120-r0 | baseline_candidate | 9062597, 9095187, 9226109 | 13693667, 13758198, 13723227 |
| builder-k1-b67109120-r0 | baseline_baseline | 9084307, 9065837, 9261529 | 9364472, 9419602, 9107937 |
| builder-k1-b67109120-r1 | baseline_candidate | 9078633, 9078172, 9106733 | 13718844, 13706254, 13704744 |
| builder-k1-b67109120-r1 | baseline_baseline | 9075183, 9085733, 9092183 | 9103142, 9085542, 9130083 |
| builder-k1-b67109120-r2 | baseline_candidate | 9272523, 9240963, 9280953 | 13732513, 13690024, 13913835 |
| builder-k1-b67109120-r2 | baseline_baseline | 9294343, 9293153, 9294474 | 9290953, 9237274, 9270433 |
| builder-k1-b67109120-r3 | baseline_candidate | 9261402, 9257322, 9249203 | 13680831, 13886692, 13692111 |
| builder-k1-b67109120-r3 | baseline_baseline | 9245772, 9282253, 9251912 | 9276972, 9285302, 9289373 |
| builder-k1-b268435200-r3 | baseline_candidate | 33854467, 34025799, 33991980 | 52680322, 52665763, 52663153 |
| builder-k1-b268435200-r3 | baseline_baseline | 33879478, 33846198, 34886810 | 33937588, 33874258, 33930969 |
| builder-k2-b67109120-r3 | baseline_candidate | 17559761, 17572812, 17551082 | 26713798, 26699658, 26723748 |
| builder-k2-b67109120-r3 | baseline_baseline | 17574590, 17558450, 17599201 | 17537751, 17545221, 17616152 |
| builder-force-host | baseline_candidate | 52781887, 52810848, 52768898 | 52777495, 52845846, 52784075 |
| builder-force-host | baseline_baseline | 52780053, 52840223, 52794723 | 52762831, 52814121, 52802752 |

The 64 MiB root-sweep paired ratios were 1.5110, 1.5098, 1.4814 and 1.4804 for roots 0,1,2,3. Near-256 MiB was 1.5493 and two keys at 64 MiB+256 was 1.5213. Their baseline/control ratios were 0.9974–1.0308. This consistent larger direct-work slowdown is substantially larger than its measured controls. Small one-key ratio was 0.9669 versus baseline/control 0.9911, below the required 10% practical reduction. The required forced-HOST case was 1.0003 versus 0.9997; its scratch/HOST charges were each 8,388,608 bytes, while the comparable direct case used 50,331,648 bytes root scratch and no HOST staging. No direct-work refusal was observed.

## SQL

Q7 completed one direct accumulated publication per measured query, with 23,982,720 published bytes, 95,930,880 input/ready bytes, one admitted key and four GPU contributors. Q21 completed three direct publications, with 174,974,624 published bytes and 699,898,496 input/ready bytes, three admitted keys and 12 cumulative GPU-participation counts (three publications across four devices), not 12 distinct GPUs. All 13 expected Q21 contributions completed. There were no cap/admission/inventory/resource/error skips, incomplete accumulations or storage leaks in these observations.

Q4 produced no accumulation attempt or publication: started/admitted/published counts and accumulated bytes were zero, all exposed accumulation-rejection counters were zero, and one membership filter was built. Its recorded reason is `no_accumulated_publication`; the snapshots do not establish why its plan had no accumulation attempt. It must not be described as a cap skip or a zero-time publication improvement. All three queries have valid end-to-end observations.

Query intervals and publication intervals are distinct. Publication timing, when available, is the sum of all finish-attempt preparation-through-completion intervals in a query; it is not necessarily query critical-path time. Publication comparisons require equivalent admitted and published work. SQL per-publication root/K/B/C are not exposed by the snapshot.

| Query | Metric | Control median ms | Candidate median ms | Control p90 ms | Candidate p90 ms | C/B median paired ratio | C/B p90 ratio | B/B median ratio | B/B p90 ratio | Excluded C/B pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | query_elapsed_ns | 58.565044 | 61.685562 | 59.481860 | 63.464620 | 1.0533 | 1.0978 | 1.0283 | 1.0805 | {} |
| sql-q4 | host_elapsed_ns | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | {"publication_work_not_equivalent_or_not_published": 3} |
| sql-q21 | query_elapsed_ns | 238.346926 | 254.443982 | 241.939674 | 255.301784 | 1.0522 | 1.0720 | 1.0211 | 1.0652 | {} |
| sql-q21 | host_elapsed_ns | 24.407922 | 35.694651 | 24.537236 | 36.347217 | 1.4672 | 1.4822 | 1.0581 | 1.0657 | {} |
| sql-q7 | query_elapsed_ns | 69.188158 | 74.117229 | 72.619183 | 74.211119 | 1.0729 | 1.0764 | 1.0467 | 1.0900 | {} |
| sql-q7 | host_elapsed_ns | 3.402383 | 5.010317 | 3.402575 | 5.229048 | 1.4725 | 1.5406 | 1.0067 | 1.0079 | {} |

Counter values per measured query execution (sets show all distinct observations):

| Query | Comparison/arm | Publication count | Published bytes | Input bytes | Ready bytes | Admitted keys | Contributing GPUs | Skip reason | Interception/profile/oracle |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | baseline_candidate/baseline | 0 | 0 | 0 | 0 | 0 | 0 | "no_accumulated_publication" | true/true/true |
| sql-q4 | baseline_candidate/candidate | 0 | 0 | 0 | 0 | 0 | 0 | "no_accumulated_publication" | true/true/true |
| sql-q4 | baseline_baseline/baseline | 0 | 0 | 0 | 0 | 0 | 0 | "no_accumulated_publication" | true/true/true |
| sql-q4 | baseline_baseline/candidate | 0 | 0 | 0 | 0 | 0 | 0 | "no_accumulated_publication" | true/true/true |
| sql-q21 | baseline_candidate/baseline | 3 | 174974624 | 699898496 | 699898496 | 3 | 12 | null | true/true/true |
| sql-q21 | baseline_candidate/candidate | 3 | 174974624 | 699898496 | 699898496 | 3 | 12 | null | true/true/true |
| sql-q21 | baseline_baseline/baseline | 3 | 174974624 | 699898496 | 699898496 | 3 | 12 | null | true/true/true |
| sql-q21 | baseline_baseline/candidate | 3 | 174974624 | 699898496 | 699898496 | 3 | 12 | null | true/true/true |
| sql-q7 | baseline_candidate/baseline | 1 | 23982720 | 95930880 | 95930880 | 1 | 4 | null | true/true/true |
| sql-q7 | baseline_candidate/candidate | 1 | 23982720 | 95930880 | 95930880 | 1 | 4 | null | true/true/true |
| sql-q7 | baseline_baseline/baseline | 1 | 23982720 | 95930880 | 95930880 | 1 | 4 | null | true/true/true |
| sql-q7 | baseline_baseline/candidate | 1 | 23982720 | 95930880 | 95930880 | 1 | 4 | null | true/true/true |

Raw measured query and publication intervals, in ns:

| Query | Comparison/arm | Query ns | Publication ns |
| --- | --- | --- | --- |
| sql-q4 | baseline_candidate/baseline | 59711064, 57632264, 58565044 | None, None, None |
| sql-q4 | baseline_candidate/candidate | 57900545, 63909384, 61685562 | None, None, None |
| sql-q4 | baseline_baseline/baseline | 58432118, 55654142, 59004884 | None, None, None |
| sql-q4 | baseline_baseline/candidate | 59749360, 60857980, 60674378 | None, None, None |
| sql-q21 | baseline_candidate/baseline | 236269245, 238346926, 242837861 | 24569564, 24407922, 24328170 |
| sql-q21 | baseline_candidate/candidate | 254443982, 249178441, 255516234 | 36510358, 35480658, 35694651 |
| sql-q21 | baseline_baseline/baseline | 233821462, 232587600, 245889363 | 23263510, 23394662, 24555624 |
| sql-q21 | baseline_baseline/candidate | 251656391, 237484280, 239971554 | 24835578, 24754777, 23248231 |
| sql-q7 | baseline_candidate/baseline | 68797684, 73476939, 69188158 | 3402623, 3392193, 3402383 |
| sql-q7 | baseline_candidate/candidate | 74117229, 73602844, 74234591 | 5010317, 5283731, 5004198 |
| sql-q7 | baseline_baseline/baseline | 70793001, 67616780, 68565699 | 3399923, 3434243, 3382582 |
| sql-q7 | baseline_baseline/candidate | 71753133, 74434109, 71765904 | 3422753, 3400183, 3410433 |

Q7/Q21 publication paired ratios were 1.4725/1.4672; baseline-control publication ratios were 1.0067/1.0581. Both publications compared equivalent work; Q4 publication comparisons each excluded all three pairs because no publication occurred. Query paired ratios were Q4 1.0533, Q21 1.0522, Q7 1.0729; corresponding baseline-control ratios were 1.0283, 1.0211, 1.0467. These query controls demonstrate non-negligible process-to-process variation. No query regression guard or confidence-interval claim is supported by three screening repetitions.

## Interpretation

Observed: actual-pool access succeeded everywhere, pair/root rates were uniform across roots and issuers, concurrent ingress used the same bytes/workspace as control, larger direct builder work slowed at every screened root, and equivalent Q7/Q21 publication also slowed. HOST behavior was effectively unchanged. This provides no evidence to justify adopting concurrent ingress, changing the coordinator/root, selecting source-issued production copies, or implementing a more complex topology now.

Hypotheses only: independent ingress streams may contend with root OR/egress or encounter a shared transport limit. Host completion timing and copy APIs cannot distinguish these explanations. No local stage events, route counters, committed-memory peaks, or profiler trace were measured. The small gain from all-pairs source-issued traffic does not establish a production publication gain; in the four-contributor case full-partial replication would move 12B instead of the existing 6B and do redundant OR work. This screen does not justify implementing that doubled-traffic scheme.

A bounded, separately labeled profiler comparison is a reasonable diagnostic if the coordinator wants to investigate the large slowdown. Held-out confirmation, a single distributed finalist, all22 guards, and final pruning remain separate decisions. The unchanged pilot has not earned confirmation as a likely improvement from this screen. Control plus bounded HOST fallback remains the conservative recommendation, not a statistically selected winner.

## Evidence and limitations

Evidence directory: `docs/experiments/s4-results/20261023T150247Z-l40s-screen`. Its [SHA256SUMS](docs/experiments/s4-results/20261023T150247Z-l40s-screen/SHA256SUMS) covers every returned evidence file; [archive-map.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/archive-map.json) records originating absolute host paths, archive prefixes, sizes and SHA256 hashes without rewriting the originating manifests. The report itself is versioned in the result commit.

| Originating host path | Returned artifact | Audit content |
| --- | --- | --- |
| /tmp/s4-four-gpu-20261023/build/s4-results/20261023T150247Z-l40s-screen | [screen-campaign.tar.gz](docs/experiments/s4-results/20261023T150247Z-l40s-screen/screen-campaign.tar.gz) | Entire campaign: native/aggregate JSONL, requests, frozen SQL/config, loader logs, machine snapshots, telemetry, experiment and completion records |
| /tmp/s4-four-gpu-20261023/build/s4-retry-xxhash | [build-records/build-manifest.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/build-records/build-manifest.json) | Both manifests, build commands/CMake caches; hashes of frozen fixture/library/extension/CLI |
| /tmp/s4-four-gpu-20261023/build/s4 | [build-attempt1-incomplete.tar.gz](docs/experiments/s4-results/20261023T150247Z-l40s-screen/build-attempt1-incomplete.tar.gz) | Initial incomplete helper output; paired-build.log and command JSON preserve failure |
| /tmp/s4-four-gpu-20261023/build/s4-retry-xxhsum | [build-attempt2-incomplete.tar.gz](docs/experiments/s4-results/20261023T150247Z-l40s-screen/build-attempt2-incomplete.tar.gz) | Second incomplete helper output; paired-build-retry-xxhsum.log preserves failure |
| Analyzer output | [screen-analysis.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/screen-analysis.json) | All workloads, raw paired observations, exclusions, controls and no selection interval |
| Additional audit outputs | [workload-summary.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/workload-summary.json) | All measured field values by workload/comparison/arm; native records remain authoritative |
| Fresh validation binaries (excluded) | [validation-artifact-hashes.json](docs/experiments/s4-results/20261023T150247Z-l40s-screen/validation-artifact-hashes.json) | Exact paths/sizes/hashes of the four test executables |
| Command/environment provenance | `*.command.json`, `*.log`, installed-packages.json, build-environment.json | Exact argv, cwd, UTC start/end, exit statuses, activated dependencies and supplemental tool hashes |

No dataset files, application binaries, environment directories or credentials are included. The two preliminary configuration failures are preserved; no failed campaign was discarded and the successful screen was not selected from repeated campaigns. Their original per-attempt CMake caches were subsequently reused by configuration and are unavailable as separate snapshots; failure logs contain the failing commands and diagnostics, and final per-build caches are returned.

The screen is complete and analyzable; all 53 scheduled workloads are represented. Remaining limits are one independent pair per comparison, baseline query variation, no held-out/all22 validation, no sanitizer rerun, no physical-route/stage/committed-peak counters, no continuously traced competing CPU load, no fully hashed SF100 corpus, and the documented reused-environment deviation. Q4 lacks an accumulated-publication observation. SQL snapshots do not expose per-publication geometry, root, byte-route totals or workspace charges.

To audit elsewhere, run `sha256sum -c SHA256SUMS` from the evidence directory, extract `screen-campaign.tar.gz` into a scratch directory, and run the committed analyzer against `build/s4-results/20261023T150247Z-l40s-screen`. The archive was compared byte-for-byte against all 614 originating files, including all 86 native JSONL files, and the analyzer was rerun after extraction to a new directory with identical results apart from directory paths (see archive-verification.json). The standalone analyzer consumes the saved screen rather than executing archived absolute binary paths. Build artifact absolute paths remain provenance; rebuilding/running on another host requires fresh manifests as specified in S4_EXPERIMENTS.md. Supplementary `s4_*.py` scripts record this run's data inspection, packaging and report generation; the committed S4 runner/analyzer and untouched native records define the experiment.
