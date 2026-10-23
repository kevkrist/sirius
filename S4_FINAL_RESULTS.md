# S4 final four-GPU validation results

## Conclusion

**Inconclusive final acceptance.** All 548 processes and 4,908 records passed native correctness checks, but only 6/22 prerequisite/final query guards and 2/3 cleanup guards were established. No query was classified as a clear regression; timing precision, baseline drift and changed common counters prevent a full non-regression claim.

Prerequisite/final assessments: `{"baseline_control_missing": 1, "inconclusive_baseline_control_drift": 1, "insufficient_evidence": 2, "regression_guard_inconclusive": 12, "regression_guard_pass": 6}`. Frozen-control/final cleanup assessments: `{"insufficient_evidence": 1, "regression_guard_pass": 2}`. These are separate comparisons: frozen control already contains S4 and cannot replace the pre-S4 prerequisite. Changed-work outcomes are explicitly excluded from overhead attribution. No losing pilot or new topology was run or implemented. The retained implementation is the original direct pipeline plus bounded HOST fallback. Final implementation acceptance and any PR decision remain with the coordinator.

Recommendation: investigate the optional filter-count variability and timing noise before another acceptance cohort; retain every current exclusion and the fixed 3% threshold.

## Reproduction

| Role | Measured source commit | Exact HEAD tree |
| --- | --- | --- |
| prerequisite | 78a3e34c66143152ff075729f4c018806c11e67d | 9f59d68e4cdc7ca47999c3579407f6f5168ea652 |
| final | abe5dfe7f06ae42b20fc71b0c5c8deeb4f37394b | 9e1f6efd6948044966c7c9747b1e963e3b45fb3d |
| control | 9b2e8201787f381b62715c01405f659b925c49a7 | df9ad08eac3c88a170996fe097745a9bfb4d5fde |

Final source root: `/home/ubuntu/sirius/build/s4-final-validation`. The branch was fetched and pulled from `origin/experiment/s4-final-validation-20261023` on `kevkrist/sirius`; the measured final HEAD stayed fixed through all builds and measurements. References are isolated persistent worktrees under `build/reference-prerequisite` and `build/reference-control`. The prerequisite production tree is exactly `9f59d68e4cdc7ca47999c3579407f6f5168ea652`; both references have only the guide's four SQL fixture files, baseline test-target CMake file and root test include overlay. No baseline production or core-target compatibility edit was made. The helper verified this before and after builds.

`S4_RESULT_REMOTE=origin`; result branch `experiment/s4-final-validation-20261023`; evidence directory `docs/experiments/s4-results/20261023T193307Z-four-gpu-final`. Final result publication occurs after measurements. Unrelated local files and modified submodule checkouts in the original workspace were preserved. The old screening report and its committed evidence were preserved unchanged.

Overlay/source identities: [source-and-overlay-identities.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/source-and-overlay-identities.json). All three native fixtures use the same SQL source hashes and Q1 oracle header. Normalized configuration SHA256 is `129c676eedbae5991c65a9b8b76fc21ada3adb3f4be856694a88429e1c358b1a` in every arm; only the frozen control's known historical `current_direct` selector cache entry is omitted by the guide's normalizer. Native revision roles are test-target metadata, not runtime algorithm selectors.

Pinned build dependencies (identical across the three revision manifests; unrelated uninitialized experimental dependencies remain listed in the full source records):

| Submodule | Pinned revision |
| --- | --- |
| .claude/claude-tools | bcfccda6e35b50f356df99c2baae16204901ede3 |
| cucascade | 1a9281093a0fe49634594d541d7ce327129f53d6 |
| duckdb | 88ecd0a49d7d3d838e002b2fbc0ce91ccf74110e |
| sirius-duckdb/duckdb | 88ecd0a49d7d3d838e002b2fbc0ce91ccf74110e |
| sirius-duckdb/extension-ci-tools | 55e9f697b4fb70f357cc1bd4d1893cbf217ee359 |
| sirius-duckdb/vcpkg | cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3 |
| substrait | 922489d3c8c77140b189ad3847554de141378b04 |
| substrait/duckdb | d8cdaa33fda8df955cc76ef58a280f68f4cd43fa |
| substrait/extension-ci-tools | 72e76e99cd7fee45a99739cd118ec2db64e034ec |
| substrait/substrait | 29844f1b58a666f36493c27def553854e81328a7 |

All three fixtures report DuckDB `v2.0.0-dev87629`.

| Artifact SHA256 | prerequisite | final | control |
| --- | --- | --- | --- |
| fixture | 1c7cca30fe96abfa57ceecfcf8bae591bd1419932cc3ba3b37bf306744a0d3a8 | dfc3a52a9d69f32e82a0abcdc2fbf03e46a3b732177fef2c8be70ba1a61e14c8 | b82d6ffe175ff46f9fe70ab58dce7e8409b63649daa3c68eb09ddf478c95a52f |
| library | d0be091e85f2e79f46bf1de74850bdbc12626846770671d2b30bdb6cc261cdaf | b1c074762b47c67ef8ba71cacf175b9a866ba86e2d9a3ee69793ce1f368205d2 | 1819aeb5983bc8ba5439bdb5abaa143bad1f56f1c6c2dc9a8798a34672cca4a6 |
| extension | ef9d3c04280a4e5032f3ec89f8fd807c1ce93f728bf643bae5a5fbc14112ef05 | ecd50ceb39986cd9d5f747a95d3eade94baebce0226dc4fc37955b1b29420c4b | c2daf53ffdd5bc75cefa8cb91d22347823e9c2870c6f39b0805981cee313aee8 |
| cli | 5163383eb7a1893a73500294abe53359f009313e9793ccefed303cde314d5539 | 4946cbd6f88be3a573b2fcdad65e7fee6f33a4f0684bfd05f657ae5a943ce827 | e6961a7c56bdc102c952889b4bac040397a4c39bf3490d56af79b2d5789938bc |

The final artifact is reused byte-for-byte by both pair manifests. All three build manifests, raw CMake caches, wrapper caches, commands and test-binary hashes are under [build-records/](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/build-records/prerequisite/manifest.json). Pair manifests are [build-records/prerequisite-build-manifest.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/build-records/prerequisite-build-manifest.json) and [build-records/control-build-manifest.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/build-records/control-build-manifest.json). Originating absolute paths remain intact. Built binaries are excluded from the commit.

Environment: existing Pixi default environment activated through `pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml`; Python 3.12.14, GCC 15.3.0, CUDA 13.4.92, cuDF 26.08.01 and RMM 26.08.00. The manifest requests Python 3.14; the available unchanged 3.12 environment was explicitly reused for all arms. Identical extra CMake flags disabled only the unrelated TPC-Havoc target, reused pinned cuco/Corrosion fetch sources, and supplied cached xxhash 0.8.3 executable/header paths. This host's architecture was `native`, 24 build jobs. Package identities, supplemental tool/source hashes and deviations are in [installed-packages.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/installed-packages.json) and [build-environment.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/build-environment.json). Dependency revisions and all exact overlay hashes are in each manifest and source identity record.

Disk-capacity procedure: each role was built and frozen separately with the supported role-only helper option, then its intermediate native/wrapper build directories were reclaimed after tests passed. Frozen artifacts and exact caches/manifests were retained. The two pairs were formed with the same already-frozen final artifact. This changes orchestration/output paths, not the source, compiler, flags, dependencies or measured artifacts. [reclaimed-own-builds.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/reclaimed-own-builds.json) records reclamation of only this task's old compiled pilot artifacts after their historical checksums were verified.

All commands below ran from the final source root. Each linked `*.command.json` also records exact argv, cwd, start/end UTC, elapsed wall time and exit status. Build stdout/stderr are complete.

```bash
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python -m unittest discover -s scripts -p test_s4_experiments.py -v
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/build_s4_experiments.py --baseline-source /home/ubuntu/sirius/build/s4-final-validation/build/reference-prerequisite --baseline-kind prerequisite --revision-role baseline --output /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/prerequisite-artifacts --cuda-architectures native --jobs 24 --cmake-option=-DSIRIUS_TPCH_HAVOC=OFF --cmake-option=-DFETCHCONTENT_SOURCE_DIR_CUCO=/home/ubuntu/sirius/build/s4-final-fetch-sources/cuco-src --cmake-option=-DFETCHCONTENT_SOURCE_DIR_CORROSION=/home/ubuntu/sirius/build/s4-final-fetch-sources/corrosion-src --cmake-option=-DXXHSUM_EXECUTABLE=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/bin/xxhsum --cmake-option=-DSIMPATICO_XXHASH_INCLUDE_DIR=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/include
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/prerequisite-artifacts/baseline/sirius_unittest '[dynamic_filter][multi_partition],[in_task_publication],[pool_peer_access],[fused_membership],[fused_consumer]' --order decl
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/prerequisite-artifacts/baseline/sirius_cuda_fault_test
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/build_s4_experiments.py --baseline-source /home/ubuntu/sirius/build/s4-final-validation/build/reference-prerequisite --baseline-kind prerequisite --revision-role final --output /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/final-artifacts --cuda-architectures native --jobs 24 --cmake-option=-DSIRIUS_TPCH_HAVOC=OFF --cmake-option=-DFETCHCONTENT_SOURCE_DIR_CUCO=/home/ubuntu/sirius/build/s4-final-fetch-sources/cuco-src --cmake-option=-DFETCHCONTENT_SOURCE_DIR_CORROSION=/home/ubuntu/sirius/build/s4-final-fetch-sources/corrosion-src --cmake-option=-DXXHSUM_EXECUTABLE=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/bin/xxhsum --cmake-option=-DSIMPATICO_XXHASH_INCLUDE_DIR=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/include
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/final-artifacts/final/sirius_unittest '[s4_publication]' --order decl
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/final-artifacts/final/sirius_unittest '[sql_oracle]' --order decl
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/final-artifacts/final/sirius_cuda_fault_test
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/build_s4_experiments.py --baseline-source /home/ubuntu/sirius/build/s4-final-validation/build/reference-control --baseline-kind s4_control --revision-role baseline --output /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/control-artifacts --cuda-architectures native --jobs 24 --cmake-option=-DSIRIUS_TPCH_HAVOC=OFF --cmake-option=-DFETCHCONTENT_SOURCE_DIR_CUCO=/home/ubuntu/sirius/build/s4-final-fetch-sources/cuco-src --cmake-option=-DFETCHCONTENT_SOURCE_DIR_CORROSION=/home/ubuntu/sirius/build/s4-final-fetch-sources/corrosion-src --cmake-option=-DXXHSUM_EXECUTABLE=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/bin/xxhsum --cmake-option=-DSIMPATICO_XXHASH_INCLUDE_DIR=/home/ubuntu/.cache/rattler/cache/pkgs/xxhash-0.8.3-hec8ed23_1/include
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/control-artifacts/baseline/sirius_unittest '[dynamic_filter][multi_partition],[in_task_publication],[pool_peer_access],[fused_membership],[fused_consumer]' --order decl
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/control-artifacts/baseline/sirius_cuda_fault_test
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/run_s4_experiments.py pair-manifests --baseline /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/prerequisite-artifacts/baseline/manifest.json --final /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/final-artifacts/final/manifest.json --output /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/prerequisite/build-manifest.json
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/run_s4_experiments.py pair-manifests --baseline /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/control-artifacts/baseline/manifest.json --final /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/final-artifacts/final/manifest.json --output /home/ubuntu/sirius/build/s4-final-validation/build/s4-revisions/control/build-manifest.json
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/run_s4_experiments.py screen --stages builder --gpus 0,1,2,3 --warmups 3 --pairs 3 --seed 1823 --build-manifest build/s4-revisions/prerequisite/build-manifest.json --profile default256 --output build/s4-final-results/20261023T193307Z-four-gpu-final/builder-screen
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/analyze_s4_experiments.py --results build/s4-final-results/20261023T193307Z-four-gpu-final/builder-screen --output /home/ubuntu/sirius/build/s4-final-validation/docs/experiments/s4-results/20261023T193307Z-four-gpu-final/builder-screen-analysis.json
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/run_s4_experiments.py confirm --stages builder --gpus 0,1,2,3 --roots 0,3 --build-manifest build/s4-revisions/prerequisite/build-manifest.json --profile default256 --warmups 3 --process-pairs 5 --pairs-per-process 6 --seed 1824 --output build/s4-final-results/20261023T193307Z-four-gpu-final/builder-heldout
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/analyze_s4_experiments.py --results build/s4-final-results/20261023T193307Z-four-gpu-final/builder-heldout --output /home/ubuntu/sirius/build/s4-final-validation/docs/experiments/s4-results/20261023T193307Z-four-gpu-final/builder-heldout-analysis.json
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/run_s4_experiments.py regression --stages sql --gpus 0,1,2,3 --queries all22 --build-manifest build/s4-revisions/prerequisite/build-manifest.json --data-path /data/tpch_parquet_sf100 --scale 100 --profile default256 --warmups 3 --process-pairs 5 --pairs-per-process 6 --seed 1825 --output build/s4-final-results/20261023T193307Z-four-gpu-final/prerequisite-all22
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/analyze_s4_experiments.py --results build/s4-final-results/20261023T193307Z-four-gpu-final/prerequisite-all22 --output /home/ubuntu/sirius/build/s4-final-validation/docs/experiments/s4-results/20261023T193307Z-four-gpu-final/prerequisite-all22-analysis.json
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/run_s4_experiments.py confirm --stages sql --gpus 0,1,2,3 --queries 4,7,21 --build-manifest build/s4-revisions/control/build-manifest.json --data-path /data/tpch_parquet_sf100 --scale 100 --profile default256 --warmups 3 --process-pairs 5 --pairs-per-process 6 --seed 1824 --output build/s4-final-results/20261023T193307Z-four-gpu-final/control-sql
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/analyze_s4_experiments.py --results build/s4-final-results/20261023T193307Z-four-gpu-final/control-sql --output /home/ubuntu/sirius/build/s4-final-validation/docs/experiments/s4-results/20261023T193307Z-four-gpu-final/control-sql-analysis.json
```

| Step | Start UTC | End UTC | Exit |
| --- | --- | --- | --- |
| harness-tests | 2026-10-23T19:35:42.918074+00:00 | 2026-10-23T19:35:43.616498+00:00 | 0 |
| build-prerequisite | 2026-10-23T19:36:38.373700+00:00 | 2026-10-23T19:44:12.368688+00:00 | 0 |
| prerequisite-cumulative | 2026-10-23T19:44:12.714267+00:00 | 2026-10-23T19:45:07.612128+00:00 | 0 |
| prerequisite-fault | 2026-10-23T19:45:07.632157+00:00 | 2026-10-23T19:45:12.952176+00:00 | 0 |
| build-final | 2026-10-23T19:45:13.655465+00:00 | 2026-10-23T19:52:10.623413+00:00 | 0 |
| final-focused | 2026-10-23T19:52:10.964047+00:00 | 2026-10-23T19:52:39.761188+00:00 | 0 |
| final-sql-oracle | 2026-10-23T19:52:39.781166+00:00 | 2026-10-23T19:52:46.539831+00:00 | 0 |
| final-fault | 2026-10-23T19:52:46.559610+00:00 | 2026-10-23T19:53:01.412002+00:00 | 0 |
| build-control | 2026-10-23T19:53:02.131823+00:00 | 2026-10-23T20:00:02.552535+00:00 | 0 |
| control-cumulative | 2026-10-23T20:00:02.896279+00:00 | 2026-10-23T20:01:19.668851+00:00 | 0 |
| control-fault | 2026-10-23T20:01:19.689037+00:00 | 2026-10-23T20:01:34.694294+00:00 | 0 |
| pair-prerequisite | 2026-10-23T20:01:35.398779+00:00 | 2026-10-23T20:01:36.342380+00:00 | 0 |
| pair-control | 2026-10-23T20:01:36.362338+00:00 | 2026-10-23T20:01:37.296063+00:00 | 0 |
| campaign-builder-screen | 2026-10-23T20:01:46.081385+00:00 | 2026-10-23T20:02:44.998682+00:00 | 0 |
| analyze-builder-screen | 2026-10-23T20:02:45.018870+00:00 | 2026-10-23T20:02:45.188952+00:00 | 0 |
| campaign-builder-heldout | 2026-10-23T20:02:45.208246+00:00 | 2026-10-23T20:08:31.713498+00:00 | 0 |
| analyze-builder-heldout | 2026-10-23T20:08:31.733137+00:00 | 2026-10-23T20:08:31.928515+00:00 | 0 |
| campaign-prerequisite-all22 | 2026-10-23T20:08:31.947899+00:00 | 2026-10-23T21:07:46.362937+00:00 | 0 |
| analyze-prerequisite-all22 | 2026-10-23T21:07:46.382496+00:00 | 2026-10-23T21:07:46.850398+00:00 | 0 |
| campaign-control-sql | 2026-10-23T21:07:46.870207+00:00 | 2026-10-23T21:16:40.020777+00:00 | 0 |
| analyze-control-sql | 2026-10-23T21:16:40.040020+00:00 | 2026-10-23T21:16:40.247663+00:00 | 0 |

Dataset: `/data/tpch_parquet_sf100`, verified SF100, 14 Parquet files. Generator metadata and independently summed Parquet footer counts agree; scale was not inferred from the directory name. Metadata SHA256: `2892cfd2cd29a17859351c3f751baa7bdb6505cacc51083f999da2c870db027e`. Full dataset content hashing was not performed; path/size/mtime identities were frozen by the runner and footer rows are separately returned. See [dataset-identity.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/dataset-identity.json) and [dataset-generator-metadata.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/dataset-generator-metadata.json).

| Table | Independent footer rows |
| --- | --- |
| customer | 15000000 |
| lineitem | 600037902 |
| nation | 25 |
| orders | 150000000 |
| part | 20000000 |
| partsupp | 80000000 |
| region | 5 |
| supplier | 1000000 |

Assigned CUDA-visible ordinals are 0,1,2,3; `CUDA_VISIBLE_DEVICES` was unset. All are L40S, with PHB topology between all distinct pairs and driver read/write peer matrices OK. Driver capability is not a physical-route measurement; final builder records establish the actual pool paths used.

| GPU | UUID | Model | PCI bus | Memory | Driver | Preflight P-state | SM/memory clocks | Power limit |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | GPU-874bc7df-e2cc-9ff8-6c5b-6c7972cf7cf0 | NVIDIA L40S | 00000002:00:01.0 | 46068 MiB | 615.71.09 | P8 | 210 MHz/405 MHz | 350.00 W |
| 1 | GPU-20d23e0a-42ef-3ed4-a307-d190b4ed0c05 | NVIDIA L40S | 00000002:00:02.0 | 46068 MiB | 615.71.09 | P8 | 210 MHz/405 MHz | 350.00 W |
| 2 | GPU-1775fd41-525a-5ff6-e62d-ac61b1651418 | NVIDIA L40S | 00000002:00:03.0 | 46068 MiB | 615.71.09 | P8 | 210 MHz/405 MHz | 350.00 W |
| 3 | GPU-9014ca3c-c056-2956-06ce-72492b72ab80 | NVIDIA L40S | 00000002:00:04.0 | 46068 MiB | 615.71.09 | P8 | 210 MHz/405 MHz | 350.00 W |

The KVM guest reports AMD EPYC 9254, 32 logical CPUs, one NUMA node, affinity CPUs 0–31 and default NUMA memory policy. No clocks or power policy were changed/locked. Idle boundary snapshots and sequential process execution were used; continuous competing CPU/GPU load and frequency telemetry were not collected. Full per-campaign before/after machine snapshots are in each archive.

Every SQL arm used default256 (268,435,456-byte combined Bloom cap per GPU, coverage 0.9), GPU pinning and compression disabled. Runner-generated YAML uses GPUs 0–3, GPU fraction 0.4, HOST fraction 0.5, two initial 128 MiB HOST pools and 1 MiB blocks; no parent two-GPU 0.85 configuration or older 0.45 coverage was reused. Builder/transport fixtures own their real-pool configuration. Screen seed 1823; held-out builder and cleanup SQL seed 1824; all22 seed 1825. Roots 0 and 3 were predeclared endpoint roots per the guide; the previous host screen did not establish a materially adverse root. Stage A was optional and was not repeated.

## Correctness

| Fresh check | Cases/tests | Assertions | Exit | Skips |
| --- | --- | --- | --- | --- |
| prerequisite-cumulative | 89 | 5798 | 0 | none reported |
| prerequisite-fault | 4 | 228 | 0 | none reported |
| final-focused | 5 | 1687 | 0 | none reported |
| final-sql-oracle | 6 | 43 | 0 | none reported |
| final-fault | 7 | 832 | 0 | none reported |
| control-cumulative | 94 | 7700 | 0 | none reported |
| control-fault | 7 | 832 | 0 | none reported |
| Python harness | 30 | unittest assertion count not emitted | 0 | none reported |

All three builds and pair identity checks passed. The reference selections were `[dynamic_filter][multi_partition],[in_task_publication],[pool_peer_access],[fused_membership],[fused_consumer] --order decl`. Final used `[s4_publication]`, `[sql_oracle]`, and its isolated CUDA-fault executable. Exact commands and native assertion summaries are retained. No additional remote sanitizer run was requested or performed; parent two-GPU checks are historical evidence, not this host's validation.

Transport/builder oracles compare every replica byte to the CPU OR of pre-publication partial snapshots, check inserted-key membership and independently check submitted bytes/workspace. Snapshots and verification are outside the host publication preparation-through-completion timer. SQL compares full typed/null-aware results with fixed `exact_except_q1_avg_disc_v1`: only recognized Q1 column 8 `avg_disc` DOUBLE permits finite absolute or relative differences up to 1e-12, after exact schema/shape and unique grouping-key checks. Every other cell remains exact. Policy and tolerances are bound to all three identify/manifests and every SQL record. No policy or production code was changed after the snapshot.

Accepted Q1 raw-digest differences retained in native records (including warmups): 180. Digests are supplementary; typed comparison defines parity. Normal-SQL GPU interception and effective-profile checks are mandatory; a failed LOAD, CPU fallback, read-view mismatch or wrong profile is not accepted timing evidence. Native fixtures statically link their own revision's production objects. Separately, `-bail -no-agent` CLI loader smoke verifies that each extension initializes its matching frozen shared library. These are distinct linkage proofs.

| Campaign | Status | Processes | Records | Warmups | Measured | Record statuses |
| --- | --- | --- | --- | --- | --- | --- |
| builder-screen | complete | 8 | 48 | 24 | 24 | {'ok': 48} |
| builder-heldout | complete | 40 | 360 | 120 | 240 | {'ok': 360} |
| prerequisite-all22 | complete | 440 | 3960 | 1320 | 2640 | {'ok': 3960} |
| control-sql | complete | 60 | 540 | 180 | 360 | {'ok': 540} |

Post-measurement stability checks matched all three source identities to their built manifests, including the exact baseline overlays; installed package metadata and reused fetch-source trees were unchanged, and all four campaign YAML configuration hashes agreed. Evidence: [dependency-stability-audit.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/dependency-stability-audit.json).

Aggregate/native identity and completion-hash audits: [campaign-audit-summary.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/campaign-audit-summary.json). Each campaign contains all warmups, native and aggregate JSONL, requests, SQL/config, loader logs, machine evidence and completion/failure records. A known installed `libcusolver.so.12` version-information warning appears in test logs; the commands and loader identity checks passed.

## Final diagnostics

Builder screen and held-out measurements are final-only; no prerequisite or control builder timing is fabricated. Screen sizes are 1 MiB+256, 64 MiB+256, 256 MiB−256 and two keys at 64 MiB+256; the middle one-key size sweeps all roots and the separate forced-HOST case uses two keys. Held-out sizes are one-key 8 MiB+256 / 128 MiB+256 and two-key 8 MiB+256 / 128 MiB−256 on roots 0,3. Every case obeys the combined default cap. Each of four contributors inserts 4,096 rows/key.

All are final-only diagnostics; no comparative baseline is invented. Each contributor inserts 4,096 rows/key. Bytes and workspace charges are distinct from allocator committed peaks. Raw measured intervals and records are in builder-raw-summary.json and the complete native archives.

| Campaign | Workload | Root | K | B/key bytes | C bytes | Contributors | Transport | Measured samples | Median ms | p90 ms | Input bytes | Ready bytes | Logical edge bytes | Aligned scratch bytes | Physical HOST bytes | D2H/H2D bytes | Statuses |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| builder-screen | builder-k1-b1048832-r3 | 3 | 1 | 1048832 | 1048832 | 4 | "direct" | 3 | 0.312674 | 0.315178 | 4195328 | 4195328 | 6292992 | 3146496 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b67109120-r0 | 0 | 1 | 67109120 | 8388608 | 4 | "direct" | 3 | 9.114460 | 9.167029 | 268436480 | 268436480 | 402654720 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b67109120-r1 | 1 | 1 | 67109120 | 8388608 | 4 | "direct" | 3 | 9.087890 | 9.103282 | 268436480 | 268436480 | 402654720 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b67109120-r2 | 2 | 1 | 67109120 | 8388608 | 4 | "direct" | 3 | 9.282252 | 9.283356 | 268436480 | 268436480 | 402654720 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b67109120-r3 | 3 | 1 | 67109120 | 8388608 | 4 | "direct" | 3 | 9.290062 | 9.290198 | 268436480 | 268436480 | 402654720 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b268435200-r3 | 3 | 1 | 268435200 | 8388608 | 4 | "direct" | 3 | 34.544858 | 35.929970 | 1073740800 | 1073740800 | 1610611200 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k2-b67109120-r3 | 3 | 2 | 67109120 | 8388608 | 4 | "direct" | 3 | 18.489313 | 18.566131 | 536872960 | 536872960 | 805309440 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-force-host | 3 | 2 | 67109120 | 8388608 | 4 | "host" | 3 | 52.938312 | 52.980431 | 536872960 | 536872960 | 805309440 | 8388608 | 8388608 | 536872960/805309440 | {'ok': 3} |
| builder-heldout | builder-k1-b8388864-r0 | 0 | 1 | 8388864 | 2097152 | 4 | "direct" | 30 | 1.407727 | 1.417512 | 33555456 | 33555456 | 50333184 | 12582912 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k1-b8388864-r3 | 3 | 1 | 8388864 | 2097152 | 4 | "direct" | 30 | 1.403132 | 1.418433 | 33555456 | 33555456 | 50333184 | 12582912 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k1-b134217984-r0 | 0 | 1 | 134217984 | 8388608 | 4 | "direct" | 30 | 17.158968 | 17.183079 | 536871936 | 536871936 | 805307904 | 50331648 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k1-b134217984-r3 | 3 | 1 | 134217984 | 8388608 | 4 | "direct" | 30 | 17.544483 | 17.593310 | 536871936 | 536871936 | 805307904 | 50331648 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k2-b8388864-r0 | 0 | 2 | 8388864 | 2097152 | 4 | "direct" | 30 | 2.508881 | 2.535594 | 67110912 | 67110912 | 100666368 | 12582912 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k2-b8388864-r3 | 3 | 2 | 8388864 | 2097152 | 4 | "direct" | 30 | 2.516755 | 2.534499 | 67110912 | 67110912 | 100666368 | 12582912 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k2-b134217472-r0 | 0 | 2 | 134217472 | 8388608 | 4 | "direct" | 30 | 33.180427 | 33.270892 | 1073739776 | 1073739776 | 1610609664 | 50331648 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k2-b134217472-r3 | 3 | 2 | 134217472 | 8388608 | 4 | "direct" | 30 | 33.933551 | 34.074693 | 1073739776 | 1073739776 | 1610609664 | 50331648 | 0 | 0/0 | {'ok': 30} |

The diagnostic timer is host preparation through completion; local CUDA stage durations were unavailable. Scratch is aligned checked-out root workspace and HOST charge is physical fixed-block staging, not allocator committed/cache peaks or total process memory. Builder returns ready replicas and has zero channel-published bytes. All raw measured intervals and full field sets are in [builder-raw-summary.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/builder-raw-summary.json).

## SQL

The all22 prerequisite comparison subsumes Q4/Q7/Q21; the separate cleanup comparison checks pruning against the already-S4 control. Each query/comparison uses five fresh process pairs, three warmups and six measured repetitions per process, randomized arm/workload order, and independent baseline/baseline controls. Ratios are final/baseline. The estimator equally weights the five process-pair mean log ratios; its two-sided 95% Student-t interval uses those five independent observations (four degrees of freedom), not thirty IID repetitions. Per-query median/p90 paired ratios and raw timing summaries remain descriptive.

Each individual slowdown upper bound must be at most 1.03 with accepted work equivalence and baseline controls; a baseline-control interval excluding 1 makes that result inconclusive. Queries are not averaged to hide regressions. Common execution/publication/contribution/filter outcomes must match; changed work is excluded from overhead attribution. The prerequisite's unsupported S4 bytes, transport and preparation-time fields remain null, even when its common publication count is positive; its older final-contribution-to-first-push latency is not substituted. A zero-publication query has no publication-speed observation.

### Prerequisite versus final: all22

Each row concerns one query and one revision comparison. Ratios are final/baseline. The 95% interval is across independent process-pair mean log ratios; baseline/baseline drift is separate. Unsupported publication counters remain null.

| Query | Clusters | Measured pairs | Baseline median ms | Final median ms | Baseline p90 ms | Final p90 ms | Median paired ratio | p90 paired ratio | Ratio 95% CI | B/B median ratio | B/B 95% CI | Individual guard | Excluded pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q1 | 5 | 30 | 179.057637 | 181.907953 | 188.438635 | 186.877684 | 1.00224 | 1.06117 | [0.97038, 1.04182] | 1.05121 | [0.98378, 1.08199] | regression_guard_inconclusive | {} |
| sql-q2 | 5 | 30 | 42.988987 | 43.513495 | 45.047960 | 45.282885 | 1.01183 | 1.06418 | [0.98232, 1.03571] | 0.98517 | [0.94438, 1.00924] | regression_guard_inconclusive | {} |
| sql-q3 | 5 | 30 | 72.630510 | 72.037872 | 75.750384 | 76.884325 | 0.99255 | 1.06570 | [0.95537, 1.03848] | 0.99551 | [0.98041, 1.04342] | regression_guard_inconclusive | {} |
| sql-q4 | 5 | 30 | 58.716544 | 58.838885 | 62.023504 | 61.291827 | 0.99507 | 1.06004 | [0.96570, 1.02831] | 0.97461 | [0.93383, 1.02857] | regression_guard_pass | {} |
| sql-q5 | 5 | 30 | 59.879635 | 59.739543 | 62.268088 | 63.642304 | 0.99951 | 1.09424 | [0.97681, 1.03902] | 0.99917 | [0.97418, 1.01821] | regression_guard_inconclusive | {} |
| sql-q6 | 5 | 30 | 40.881115 | 41.048954 | 42.576554 | 43.771645 | 1.02728 | 1.11154 | [0.97637, 1.06734] | 0.99903 | [0.95275, 1.06380] | regression_guard_inconclusive | {} |
| sql-q7 | 5 | 29 | 70.476443 | 71.236982 | 72.764082 | 74.965313 | 1.01255 | 1.08960 | [0.96518, 1.06500] | 0.99186 | [0.97099, 1.01039] | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 1} |
| sql-q8 | 5 | 30 | 79.314545 | 76.473767 | 89.005057 | 80.699616 | 0.95251 | 1.03997 | [0.93200, 0.97700] | 0.97609 | [0.95108, 1.01550] | baseline_control_missing | {} |
| sql-q9 | 5 | 17 | 160.077206 | 161.626739 | 168.292285 | 168.692177 | 0.99332 | 1.07293 | [0.96891, 1.03217] | 1.01580 | [0.97945, 1.04156] | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 13} |
| sql-q10 | 5 | 30 | 149.161842 | 150.560037 | 153.918426 | 153.936038 | 1.00826 | 1.04164 | [0.99246, 1.01773] | 1.00058 | [0.98071, 1.02328] | regression_guard_pass | {} |
| sql-q11 | 5 | 30 | 27.082148 | 26.833424 | 28.100599 | 27.974794 | 1.00420 | 1.07125 | [0.96346, 1.02938] | 0.97267 | [0.94979, 0.99129] | inconclusive_baseline_control_drift | {} |
| sql-q12 | 5 | 30 | 60.846136 | 61.459991 | 64.852867 | 65.263229 | 1.01855 | 1.10000 | [0.98307, 1.03173] | 0.99388 | [0.94495, 1.04763] | regression_guard_inconclusive | {} |
| sql-q13 | 5 | 30 | 42.442540 | 43.099245 | 44.111782 | 45.316969 | 1.02043 | 1.09537 | [0.98311, 1.05886] | 0.99785 | [0.99800, 1.02029] | regression_guard_inconclusive | {} |
| sql-q14 | 5 | 30 | 62.732569 | 61.764870 | 66.337299 | 63.025046 | 0.97024 | 1.01869 | [0.94486, 0.99579] | 0.98737 | [0.96077, 1.02566] | regression_guard_pass | {} |
| sql-q15 | 5 | 30 | 65.041758 | 66.374076 | 69.679425 | 69.205482 | 1.00854 | 1.10064 | [0.94890, 1.08148] | 0.99948 | [0.98274, 1.03169] | regression_guard_inconclusive | {} |
| sql-q16 | 5 | 30 | 63.376195 | 63.503237 | 64.504211 | 64.104963 | 1.00301 | 1.02303 | [0.99662, 1.00761] | 0.99990 | [0.98864, 1.01087] | regression_guard_pass | {} |
| sql-q17 | 5 | 30 | 33.984461 | 34.795876 | 35.919853 | 36.634687 | 1.02087 | 1.06893 | [0.99424, 1.04622] | 0.99082 | [0.94741, 1.02597] | regression_guard_inconclusive | {} |
| sql-q18 | 5 | 30 | 212.648591 | 214.344733 | 218.585040 | 221.999639 | 1.01629 | 1.05323 | [0.99232, 1.03127] | 1.01063 | [0.98268, 1.01325] | regression_guard_inconclusive | {} |
| sql-q19 | 5 | 30 | 85.871539 | 85.524915 | 88.992795 | 89.012480 | 0.98994 | 1.04998 | [0.94496, 1.03999] | 1.01823 | [0.96317, 1.06430] | regression_guard_inconclusive | {} |
| sql-q20 | 5 | 30 | 47.757329 | 47.818975 | 50.506862 | 50.187352 | 1.00440 | 1.06815 | [0.98640, 1.02179] | 1.00787 | [0.98582, 1.03602] | regression_guard_pass | {} |
| sql-q21 | 5 | 30 | 243.262473 | 245.053731 | 247.909115 | 253.421453 | 1.01966 | 1.05797 | [0.98723, 1.03767] | 0.99528 | [0.97408, 1.02490] | regression_guard_inconclusive | {} |
| sql-q22 | 5 | 30 | 47.411917 | 47.745145 | 48.534735 | 49.236424 | 1.01039 | 1.04942 | [0.98933, 1.02861] | 1.00339 | [0.97923, 1.02582] | regression_guard_pass | {} |

Publication fields for baseline/final arms (distinct per-query values; contributing-GPU counts accumulate across publications):

| Query | Arm | Publication count | Metrics supported | Published bytes | Input bytes | Ready bytes | Admitted keys | GPU participation count | Publication median ms | Skip reason |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q1 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q1 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q2 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q2 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q3 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q3 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q4 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q4 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q5 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q5 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q6 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q6 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q7 | baseline | 1 | false | null | null | null | null | null | unavailable | null |
| sql-q7 | candidate | 1 | true | 23982720 | 95930880 | 95930880 | 1 | 4 | 3.402358 | null |
| sql-q8 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q8 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q9 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q9 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q10 | baseline | 1 | false | null | null | null | null | null | unavailable | null |
| sql-q10 | candidate | 1 | true | 11474848 | 45899392 | 45899392 | 1 | 4 | 1.843553 | null |
| sql-q11 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q11 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q12 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q12 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q13 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q13 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q14 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q14 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q15 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q15 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q16 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q16 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q17 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q17 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q18 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q18 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q19 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q19 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q20 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q20 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q21 | baseline | 3 | false | null | null | null | null | null | unavailable | null |
| sql-q21 | candidate | 3 | true | 174974624 | 699898496 | 699898496 | 3 | 12 | 24.542715 | null |
| sql-q22 | baseline | 0 | false | null | null | null | null | null | unavailable | "no_accumulated_publication" |
| sql-q22 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |

Publication interval comparisons are supported only when both revisions expose preparation timing and equivalent publication occurred. Prerequisite fields are unsupported, and Q4 has no publication observation. These rows cannot replace the end-to-end query guard.

| Query | Clusters | Measured pairs | Baseline median ms | Final median ms | Baseline p90 ms | Final p90 ms | Median paired ratio | p90 paired ratio | Ratio 95% CI | B/B median ratio | B/B 95% CI | Assessment | Excluded pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q1 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q2 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q3 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q4 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q5 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q6 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q7 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 1, 'publication_work_not_equivalent_or_not_published': 29} |
| sql-q8 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q9 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 13, 'publication_work_not_equivalent_or_not_published': 17} |
| sql-q10 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q11 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q12 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q13 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q14 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q15 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q16 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q17 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q18 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q19 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q20 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q21 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q22 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |

### Frozen S4 control versus final: cleanup

Each row concerns one query and one revision comparison. Ratios are final/baseline. The 95% interval is across independent process-pair mean log ratios; baseline/baseline drift is separate. Unsupported publication counters remain null.

| Query | Clusters | Measured pairs | Baseline median ms | Final median ms | Baseline p90 ms | Final p90 ms | Median paired ratio | p90 paired ratio | Ratio 95% CI | B/B median ratio | B/B 95% CI | Individual guard | Excluded pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | 5 | 30 | 57.910591 | 57.341188 | 60.193656 | 60.303248 | 0.98863 | 1.10027 | [0.96208, 1.02979] | 1.00618 | [0.99793, 1.01850] | regression_guard_pass | {} |
| sql-q7 | 5 | 29 | 71.108113 | 71.216298 | 73.016453 | 73.291501 | 0.99388 | 1.03893 | [0.96746, 1.02203] | 0.98756 | [0.94125, 1.03738] | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 1} |
| sql-q21 | 5 | 30 | 239.220514 | 241.568385 | 247.227192 | 249.832658 | 1.00090 | 1.05785 | [1.00225, 1.02224] | 1.00831 | [0.97177, 1.03415] | regression_guard_pass | {} |

Publication fields for baseline/final arms (distinct per-query values; contributing-GPU counts accumulate across publications):

| Query | Arm | Publication count | Metrics supported | Published bytes | Input bytes | Ready bytes | Admitted keys | GPU participation count | Publication median ms | Skip reason |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | baseline | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q4 | candidate | 0 | true | 0 | 0 | 0 | 0 | 0 | unavailable | "no_accumulated_publication" |
| sql-q7 | baseline | 1 | true | 23982720 | 95930880 | 95930880 | 1 | 4 | 3.408461 | null |
| sql-q7 | candidate | 1 | true | 23982720 | 95930880 | 95930880 | 1 | 4 | 3.399666 | null |
| sql-q21 | baseline | 3 | true | 174974624 | 699898496 | 699898496 | 3 | 12 | 24.646927 | null |
| sql-q21 | candidate | 3 | true | 174974624 | 699898496 | 699898496 | 3 | 12 | 24.333917 | null |

Publication interval comparisons are supported only when both revisions expose preparation timing and equivalent publication occurred. Prerequisite fields are unsupported, and Q4 has no publication observation. These rows cannot replace the end-to-end query guard.

| Query | Clusters | Measured pairs | Baseline median ms | Final median ms | Baseline p90 ms | Final p90 ms | Median paired ratio | p90 paired ratio | Ratio 95% CI | B/B median ratio | B/B 95% CI | Assessment | Excluded pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | 0 | 0 | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | insufficient_evidence | {'publication_work_not_equivalent_or_not_published': 30} |
| sql-q7 | 5 | 29 | 3.410741 | 3.398601 | 3.436107 | 3.414099 | 0.99878 | 1.00596 | [0.99412, 0.99994] | 0.99932 | [0.99160, 1.00428] | insufficient_evidence | {'common_sql_outcomes_changed_or_missing': 1} |
| sql-q21 | 5 | 30 | 24.646927 | 24.333917 | 24.988751 | 24.777928 | 0.99605 | 1.04064 | [0.98060, 1.00149] | 1.00226 | [0.98453, 1.02955] | publication_timing_observation | {} |

The original prerequisite campaign established 6/22 guards. Q7 had one baseline/final excluded pair; Q9 had 13. Baseline/baseline Q8 had two excluded pairs and Q9 had 13. Every differing common field was `filters_pushed`; other compared common outcomes, typed parity, interception and profile checks passed. Q7 examples were 5 versus 4 filters, Q8 controls 5 versus 4 or 4 versus 5, and Q9 2 versus 3 or 3 versus 2. This is counter-defined changed work, not a wrong SQL result or proof that S4 uniquely caused it. Q11 had significant baseline drift. Twelve other query guards remained precision-inconclusive. See [prerequisite-work-differences.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/prerequisite-work-differences.json).

No all22 extension was run: a linked five-cluster extension preserves the original excluded pairs and cannot make these comparisons complete. Investigate the common-outcome variability before another all22 cohort; do not drop the counter, discard pairs or relax the 3% threshold. Decision: [prerequisite-extension-decision.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/prerequisite-extension-decision.json).

The cleanup comparison established Q4 and Q21 guards. Q7 would have had a query-time upper bound below 1.03 on its remaining observations, but its original baseline/final and baseline/baseline cohorts each contain one `filters_pushed` mismatch (5/4 and 4/5). Its complete-comparison guard is therefore insufficient evidence. No cleanup extension was run, for the same retained-exclusion reason; see [control-work-differences.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/control-work-differences.json) and [control-extension-decision.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/control-extension-decision.json).

Per-query work fields, publication counts/bytes, exclusions, baseline controls and all raw measured query/publication intervals are in [sql-work-summary.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/sql-work-summary.json) and the analyzer reports. Preparation timing, when supported and positive, sums finish-attempt intervals and is not necessarily query critical-path time. SQL snapshots do not expose each publication's root/K/B/C or workspace.

Final Q7/Q21 positive-publication check:

| Campaign | Query | Measured final executions | Publication counts | Published bytes | All positive |
| --- | --- | --- | --- | --- | --- |
| prerequisite-all22 | q7 | 30 | [1] | [23982720] | True |
| prerequisite-all22 | q21 | 30 | [3] | [174974624] | True |
| control-sql | q7 | 30 | [1] | [23982720] | True |
| control-sql | q21 | 30 | [3] | [174974624] | True |

## Limits

Assessment counts: `{"control-sql": {"insufficient_evidence": 1, "regression_guard_pass": 2}, "prerequisite-all22": {"baseline_control_missing": 1, "inconclusive_baseline_control_drift": 1, "insufficient_evidence": 2, "regression_guard_inconclusive": 12, "regression_guard_pass": 6}}`. Queries with observed regression labels: `[]`; changed-work query outcomes: `[["prerequisite-all22", "sql-q7"], ["prerequisite-all22", "sql-q9"], ["control-sql", "sql-q7"]]`. Every remaining non-pass is explicit in the query tables; successful correctness alone does not establish its performance guard. No rejected pilot, topology search, source tuning, raised-cap/SF1000 run or PR was part of this task.

Unmeasured: physical transfer routes, local CUDA stage-event durations, pinned-memory NUMA placement, per-device allocator peak/delta and committed/cache peaks, continuous competing load and continuous clocks/power. Overlapping device event times were not added. Dataset contents were not fully hashed. Automatic missing-peer fallback was not manufactured on this peer-capable host; the required positive forced-HOST case and available-pool direct paths are explicitly labeled.

Environment and orchestration deviations are recorded above: unchanged existing Python 3.12 Pixi environment, explicit cached xxhash paths, supported role-only builds, and reclamation of only intermediate generated outputs after freezing/correctness. Sources, fixtures, dependencies, effective configuration and frozen binaries remained fixed during measurements. No failure or unsuccessful campaign is discarded; retry/extension disposition, if any, is recorded in the returned progress/analysis files.

Evidence is under `docs/experiments/s4-results/20261023T193307Z-four-gpu-final`. [archive-map.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/archive-map.json) maps original absolute campaign paths to returned compressed archives and their unchanged relative prefixes; [archive-files.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/archive-files.json) records every archived file hash. [archive-audit.json](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/archive-audit.json) verifies complete byte preservation and matching reanalysis after extraction using [the exact measured analyzer](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/measured-harness/analyze_s4_experiments.py) and its matching runner. [SHA256SUMS](docs/experiments/s4-results/20261023T193307Z-four-gpu-final/SHA256SUMS) covers all returned evidence and this report. Manifests were not rewritten for relocation. Reassemble any ordered split `.tar.gz.partNNN` files before extracting, then run the preserved analyzer on each extracted campaign.

No datasets, application binaries, environment directories or credentials are committed. Built artifact hashes, commands, raw caches, fixture-overlay source and measured harness files are included. To reproduce runtime checks, rebuild the exact revisions with the allowlisted overlay and saved environment/options, regenerate manifests for the new paths, and use the immutable campaign arguments. To audit results without this host, verify checksums, extract the archives and rerun the matching analyzer; absolute artifact paths remain provenance and are not executed by offline analysis.
