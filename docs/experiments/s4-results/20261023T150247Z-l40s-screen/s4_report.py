import pathlib,json,shlex,datetime
root=pathlib.Path('/tmp/s4-four-gpu-20261023'); rel=pathlib.Path('docs/experiments/s4-results/20261023T150247Z-l40s-screen'); e=root/rel; campaign=root/'build/s4-results/20261023T150247Z-l40s-screen'; run=json.loads((campaign/'experiment.json').read_text()); builds=run['builds']; screen=json.loads((e/'screen.command.json').read_text()); dataset=json.loads((e/'dataset-identity.json').read_text()); before=json.loads((campaign/'machine-before.json').read_text()); after=json.loads((campaign/'machine-after.json').read_text())
def link(name,label=None):return f'[{label or name}]({rel}/{name})'
def table(head,rows):return '\n'.join(['| '+' | '.join(head)+' |','| '+' | '.join('---' for h in head)+' |']+['| '+' | '.join(str(x) for x in row)+' |' for row in rows])+'\n'
text=f'''# S4 four-GPU screening results

## Conclusion

**Retain control (`current_direct`, including bounded HOST fallback).** The complete four-L40S/SF100/default256 screen passed correctness and provenance checks. Concurrent ingress increased median paired builder publication time by **48.0–54.9%** on the 64 MiB/root sweep, near-256 MiB case, and two-key case. SQL Q7/Q21 publication intervals increased **47.2%/46.7%** for equivalent admitted and published work. The small one-key case improved only 3.3%; forced HOST was effectively unchanged (+0.03%).

This is a screening observation, **not a four-GPU topology winner or a final production selection**. Each comparison has one independent process pair with three measured repetitions, so there is no selection confidence interval. End-to-end query ratios also increased, but baseline controls drifted by 2.1–4.7%; these observations cannot establish precise query regressions or satisfy the held-out 3% regression guard.

Recommendation: do not advance the unchanged concurrent-ingress pilot to held-out confirmation on the strength of this screen. Keep control plus required HOST fallback while the coordinator reviews this evidence. If further diagnosis is useful, run a separate bounded profiler comparison at one-key 64 MiB+256 on roots 0 and 3, and Q21, to inspect ingress/OR/egress overlap and contention. The cause of the slowdown is unmeasured; no source fix, new topology, confirmation, all22 run, or pruning was performed.

## Reproduction

- Source commit: `9b2e8201787f381b62715c01405f659b925c49a7`, checked out unchanged through all measurements.
- Prerequisite tree: `9f59d68e4cdc7ca47999c3579407f6f5168ea652`; `git cat-file -t` returned `tree`. It is the tree of merge `78a3e34c66143152ff075729f4c018806c11e67d`.
- Isolated source/build root: `/tmp/s4-four-gpu-20261023`. The original `/home/ubuntu/sirius` checkout and its unrelated files/submodule checkouts were preserved. Tracked-source status was clean before building and after measurements; evidence/report files are the only intended additions.
- Evidence run ID: `20261023T150247Z-l40s-screen`; native campaign ID: `{run['run_id']}`.
- `S4_EVIDENCE_DIR={e}` in the campaign wrapper.
- Screen UTC: **{screen['start_utc']}** to **{screen['end_utc']}** ({screen['elapsed_seconds']:.3f} s). Machine snapshots: `{before['utc']}` to `{after['utc']}`. Preparation/build/test command timestamps are in each `*.command.json`.
- Seed 1823; four explicit GPU IDs `0,1,2,3`; default256 cap 268,435,456 bytes/GPU, coverage 0.9; GPU pinning, compression disabled; three warmups and three measured repetitions per process. Randomized matched arm order and baseline/baseline controls came from the frozen runner manifest.
- Dataset: `/data/tpch_parquet_sf100`, {len(dataset['files'])} Parquet files. Scale 100 is supported by generator metadata and independently summed Parquet footer counts, not its directory name. Content hashes of the complete dataset were not measured. File paths/sizes/mtimes and footer counts are in {link('dataset-identity.json')}; generator metadata is in {link('dataset-generator-metadata.json')}.

'''
text+=table(['Table','Independently counted rows'],list(dataset['row_counts'].items()))
text+='\nRequired build-submodule initialization completed successfully:\n\n```bash\ngit submodule update --init --recursive -- \\\n  duckdb cucascade substrait .claude/claude-tools \\\n  sirius-duckdb/vcpkg sirius-duckdb/duckdb sirius-duckdb/extension-ci-tools\n```\n\n'
text+='Pinned revisions used (the complete recursive status, including intentionally uninitialized unrelated experimental submodules, is in '+link('dependency-revisions.log')+'):\n\n'
text+=table(['Dependency','Revision'],[
['duckdb and sirius-duckdb/duckdb','88ecd0a49d7d3d838e002b2fbc0ce91ccf74110e'],['cucascade','1a9281093a0fe49634594d541d7ce327129f53d6'],['substrait','922489d3c8c77140b189ad3847554de141378b04'],['.claude/claude-tools','bcfccda6e35b50f356df99c2baae16204901ede3'],['sirius-duckdb/vcpkg','cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3'],['sirius-duckdb/extension-ci-tools','55e9f697b4fb70f357cc1bd4d1893cbf217ee359'],['substrait/duckdb','d8cdaa33fda8df955cc76ef58a280f68f4cd43fa'],['substrait/extension-ci-tools','72e76e99cd7fee45a99739cd118ec2db64e034ec'],['substrait/substrait','29844f1b58a666f36493c27def553854e81328a7']])
text+='\nExact successful build, test, screen, and analyzer invocations follow. All commands ran from the isolated source root; the shared Pixi environment was explicitly activated through `pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml`.\n\n```bash\n'
text+=shlex.join(json.loads((e/'paired-build-retry-xxhash.command.json').read_text())['argv'])+'\n'
for s in ('current_direct','concurrent_ingress'):
 for t in ('focused','fault'):text+=shlex.join(json.loads((e/f'{s}-{t}.command.json').read_text())['argv'])+'\n'
for label in ('harness-tests','runner-help','analyzer-help','screen','analyze-screen'):text+=shlex.join(json.loads((e/f'{label}.command.json').read_text())['argv'])+'\n'
text+='```\n\nEnvironment deviation: the existing activated environment has Python 3.12.14 although the manifest requests Python 3.14. It has CUDA 13.4.92, GCC 15.3.0, cuDF 26.08.01 and RMM 26.08.00. It was reused unchanged rather than re-resolved. Both schedules used the same dependencies. The first configuration failed because `xxhsum` was absent from this environment; a first retry located the cached executable but also needed `xxhash.h`. Both failed helper attempts returned 1 and are preserved. The successful retry used explicit cached xxhash 0.8.3 executable/header paths in both configurations and a new output directory, `build/s4-retry-xxhash`. No measured source change was made. This explains the deviation from the guide\'s initial `build/s4` artifact path.\n\n'
text+='Installed packages and supplemental tool hashes: '+link('installed-packages.json')+', '+link('build-environment.json')+'. Both build manifests, commands and caches: '+link('build-records/build-manifest.json')+'. Compiler architecture was detected using `native` on the L40S host. The paired source SHA256 is `'+builds['current_direct']['source_sha256']+'`; normalized configuration SHA256 is `'+builds['current_direct']['configuration_sha256']+'` in both builds.\n\n'
text+=table(['Artifact SHA256','current_direct','concurrent_ingress'],[[name,builds['current_direct']['artifacts'][name]['sha256'],builds['concurrent_ingress']['artifacts'][name]['sha256']] for name in ('fixture','library','extension','cli')])
text+='''
## Machine

CUDA visibility mapping was unset, so requested ordinals 0–3 are the driver ordinals below. All GPUs are NVIDIA L40S with 46,068 MiB visible memory and 350 W power caps; persistence mode was enabled. Driver/KMD is 615.71.09; toolkit is CUDA 13.4.92. GPU clocks were not locked or modified. Preflight clocks were 210 MHz SM / 405 MHz memory (P8); full before/after samples include observed clocks and power states.

'''
gpurows=[]
for line in before['observations']['gpus']['stdout'].splitlines()[1:]:
 parts=[x.strip() for x in line.split(',')];gpurows.append(parts[:5]+[parts[7]+'/'+parts[8],parts[6]])
text+=table(['Ordinal','UUID','Model','PCI bus','Memory','Screen-start SM/memory clocks','P-state'],gpurows)
text+='''
All off-diagonal topology entries are PHB, with CPU affinity 0–31 / NUMA 0. Driver peer-read and peer-write matrices report OK for all 12 ordered pairs. Those matrices establish driver capability only; Stage A supplies byte-checked evidence from actual owning pools. Physical transfer routes remain unmeasured.

The KVM guest reports 32 logical CPUs, AMD EPYC 9254 24-Core Processor, one NUMA node; process CPU affinity and default memory policy span CPUs 0–31 / node 0. The preflight and campaign-boundary GPU snapshots showed no competing GPU process. Experiment processes were sequential and the campaign owned the observed GPU work; competing host load was not continuously traced. No external idle-window reservation or continuous clock/power telemetry was available. The sandbox itself cannot see GPU devices; native builds/tests/screen ran outside it.

Runner-generated configuration selected GPUs 0–3, GPU capacity fraction 0.4, HOST fraction 0.5, two initial 128 MiB HOST pools and 1 MiB HOST blocks. The same frozen configuration was used by both builds' SQL and loader processes. Native transport/builder modes use their fixture-owned real pools, as described by their requests. The native fixture identifies static `sirius_core` linkage; the external extension/CLI load check is a distinct loading path.

Nsight Systems 2024.5.1 was discoverable but no profiler trace was collected. Physical-route counters, local CUDA stage-event durations, actual pinned-memory NUMA placement, per-device allocator peak/delta and allocator committed/cache peaks were not measured. Workspace charges below are not total process memory or allocator committed peaks. Full machine observations are archived as `machine-before.json`, `machine-after.json`, and returned separately in the preflight snapshot.

## Correctness

'''
text+=table(['Build/check','Cases/tests','Assertions','Exit','Skips'],[
['current_direct focused [s4_publication]',5,1687,0,'none reported'],['current_direct isolated fault',7,832,0,'none reported'],['concurrent_ingress focused [s4_publication]',5,1687,0,'none reported'],['concurrent_ingress isolated fault',7,832,0,'none reported'],['Python harness unittest suite',20,'unittest assertion count not emitted',0,'none'],['runner/analyzer --help','2 commands','not applicable',0,'none'],['paired build + identity verification','both frozen schedules','not applicable',0,'none'],['screen / analyzer / independent raw verification','86 processes / 516 records','native oracles, not Catch assertion counts',0,'none']])
text+='''
Focused checks covered canonical union/replicas and workspace accounting, local-only staging, HOST exhaustion with unchanged partials, scratch reservation accounting, and publication-byte counting. Fault tests exercised their existing isolated injected-error/delay paths. These are fresh host results, not the historical 94-case suites. No new sanitizer run, all22 regression or full unit-suite pass is claimed.

The complete campaign contains 258 warmups and 258 measured records: 252 transport, 192 builder and 72 SQL records including warmups. All 516 have `status=ok` and passed oracles. There were no unavailable/skipped workloads, cap/resource refusals, failed campaign processes or failed GPU-interception/profile checks. Transport checked every destination byte and immutable sources. Builder checked the CPU OR of all pre-publication partial bytes against every replica and checked inserted-key membership with no false negatives; all builder records report complete byte accounting. SQL used exact full, type/null-aware CPU result comparison outside GPU timing, rather than a digest-only oracle.

All 72 SQL records report verified default profile, GPU interception, one successful execution/rebind, and zero planner/runtime fallbacks or read-view mismatches. Q4/Q21/Q7 returned 5/100/4 rows respectively with CPU parity. Q7/Q21 work counters are identical between schedules and matched controls; Q4 has no accumulated publication in either arm.

The helper verified distinct fixture/library hashes, compiled schedules, equal source/configuration, CLI version, and extension/library resolutions. Both CLI smoke processes used `-bail -no-agent`, reached `s4_extension_loaded`, and their loader diagnostics identify their own frozen library. Successful SQL output alone was not used as interception proof. The independent check also verified completion hashes and equality of all aggregate records to untouched native JSONL after removing harness metadata. No mismatch was found.

'''
text+='Per-command logs/counts are in '+link('current_direct-focused.log')+', '+link('current_direct-fault.log')+', '+link('concurrent_ingress-focused.log')+', '+link('concurrent_ingress-fault.log')+', '+link('harness-tests.log')+' and '+link('independent-bundle-verification.json')+'. A loader warning about missing version information in the installed `libcusolver.so.12` was present in the test logs; all tests and library-resolution checks passed.\n\n'
text+='''## Stage A

All 42 scheduled workloads completed, each with three warmups and three measured samples. All 12 ordered pairs, all-pairs concurrency, and inbound/outbound concurrency at each of four roots were tested in both issuing directions using real owning-pool allocations and grants. Each edge carried 67,109,120 logical bytes. Pair/root/all-pairs totals were respectively 67,109,120 / 201,327,360 / 805,309,440 bytes per repetition. Copies reported `cudaMemcpyPeerAsync`; issuer/stream ownership was recorded, without a physical-route claim. No direct access was unavailable.

Across measured samples, isolated pairs delivered 26.126–26.239 logical GB/s; root patterns delivered 26.277–26.361 GB/s. Median concurrent all-pairs rates were 36.721 GB/s for destination reads and 38.603 GB/s for source writes. Root patterns were near 26.3 GB/s in both directions. There is no material root-specific or directional disadvantage visible in these uninstrumented diagnostics. Aggregate all-pairs throughput did not approach four times one pair's throughput; this is shared-capacity evidence, not identification of a physical bottleneck.

The table includes every workload and all measured host intervals. Edge totals are counted once. Overlapping device-event times were not collected or added together.

'''+(e/'stage-a-table.md').read_text()+'\n'
text+='## Builder\n\n'+(e/'builder-tables.md').read_text()+'\n'
text+='''The 64 MiB root-sweep paired ratios were 1.5110, 1.5098, 1.4814 and 1.4804 for roots 0,1,2,3. Near-256 MiB was 1.5493 and two keys at 64 MiB+256 was 1.5213. Their baseline/control ratios were 0.9974–1.0308. This consistent larger direct-work slowdown is substantially larger than its measured controls. Small one-key ratio was 0.9669 versus baseline/control 0.9911, below the required 10% practical reduction. The required forced-HOST case was 1.0003 versus 0.9997; its scratch/HOST charges were each 8,388,608 bytes, while the comparable direct case used 50,331,648 bytes root scratch and no HOST staging. No direct-work refusal was observed.

## SQL

Q7 completed one direct accumulated publication per measured query, with 23,982,720 published bytes, 95,930,880 input/ready bytes, one admitted key and four GPU contributors. Q21 completed three direct publications, with 174,974,624 published bytes and 699,898,496 input/ready bytes, three admitted keys and 12 cumulative GPU-participation counts (three publications across four devices), not 12 distinct GPUs. All 13 expected Q21 contributions completed. There were no cap/admission/inventory/resource/error skips, incomplete accumulations or storage leaks in these observations.

Q4 produced no accumulation attempt or publication: started/admitted/published counts and accumulated bytes were zero, all exposed accumulation-rejection counters were zero, and one membership filter was built. Its recorded reason is `no_accumulated_publication`; the snapshots do not establish why its plan had no accumulation attempt. It must not be described as a cap skip or a zero-time publication improvement. All three queries have valid end-to-end observations.

'''+(e/'sql-tables.md').read_text()+'\n'
text+='''Q7/Q21 publication paired ratios were 1.4725/1.4672; baseline-control publication ratios were 1.0067/1.0581. Both publications compared equivalent work; Q4 publication comparisons each excluded all three pairs because no publication occurred. Query paired ratios were Q4 1.0533, Q21 1.0522, Q7 1.0729; corresponding baseline-control ratios were 1.0283, 1.0211, 1.0467. These query controls demonstrate non-negligible process-to-process variation. No query regression guard or confidence-interval claim is supported by three screening repetitions.

## Interpretation

Observed: actual-pool access succeeded everywhere, pair/root rates were uniform across roots and issuers, concurrent ingress used the same bytes/workspace as control, larger direct builder work slowed at every screened root, and equivalent Q7/Q21 publication also slowed. HOST behavior was effectively unchanged. This provides no evidence to justify adopting concurrent ingress, changing the coordinator/root, selecting source-issued production copies, or implementing a more complex topology now.

Hypotheses only: independent ingress streams may contend with root OR/egress or encounter a shared transport limit. Host completion timing and copy APIs cannot distinguish these explanations. No local stage events, route counters, committed-memory peaks, or profiler trace were measured. The small gain from all-pairs source-issued traffic does not establish a production publication gain; in the four-contributor case full-partial replication would move 12B instead of the existing 6B and do redundant OR work. This screen does not justify implementing that doubled-traffic scheme.

A bounded, separately labeled profiler comparison is a reasonable diagnostic if the coordinator wants to investigate the large slowdown. Held-out confirmation, a single distributed finalist, all22 guards, and final pruning remain separate decisions. The unchanged pilot has not earned confirmation as a likely improvement from this screen. Control plus bounded HOST fallback remains the conservative recommendation, not a statistically selected winner.

## Evidence and limitations

'''
text+='Evidence directory: `'+str(rel)+'`. Its '+link('SHA256SUMS')+' covers every returned evidence file; '+link('archive-map.json')+' records originating absolute host paths, archive prefixes, sizes and SHA256 hashes without rewriting the originating manifests. The report itself is versioned in the result commit.\n\n'
text+=table(['Originating host path','Returned artifact','Audit content'],[
[str(campaign),link('screen-campaign.tar.gz'),'Entire campaign: native/aggregate JSONL, requests, frozen SQL/config, loader logs, machine snapshots, telemetry, experiment and completion records'],[str(root/'build/s4-retry-xxhash'),link('build-records/build-manifest.json'),'Both manifests, build commands/CMake caches; hashes of frozen fixture/library/extension/CLI'],[str(root/'build/s4'),link('build-attempt1-incomplete.tar.gz'),'Initial incomplete helper output; paired-build.log and command JSON preserve failure'],[str(root/'build/s4-retry-xxhsum'),link('build-attempt2-incomplete.tar.gz'),'Second incomplete helper output; paired-build-retry-xxhsum.log preserves failure'],['Analyzer output',link('screen-analysis.json'),'All workloads, raw paired observations, exclusions, controls and no selection interval'],['Additional audit outputs',link('workload-summary.json'),'All measured field values by workload/comparison/arm; native records remain authoritative'],['Fresh validation binaries (excluded)',link('validation-artifact-hashes.json'),'Exact paths/sizes/hashes of the four test executables'],['Command/environment provenance','`*.command.json`, `*.log`, installed-packages.json, build-environment.json','Exact argv, cwd, UTC start/end, exit statuses, activated dependencies and supplemental tool hashes']])
text+='''
No dataset files, application binaries, environment directories or credentials are included. The two preliminary configuration failures are preserved; no failed campaign was discarded and the successful screen was not selected from repeated campaigns. Their original per-attempt CMake caches were subsequently reused by configuration and are unavailable as separate snapshots; failure logs contain the failing commands and diagnostics, and final per-build caches are returned.

The screen is complete and analyzable; all 53 scheduled workloads are represented. Remaining limits are one independent pair per comparison, baseline query variation, no held-out/all22 validation, no sanitizer rerun, no physical-route/stage/committed-peak counters, no continuously traced competing CPU load, no fully hashed SF100 corpus, and the documented reused-environment deviation. Q4 lacks an accumulated-publication observation. SQL snapshots do not expose per-publication geometry, root, byte-route totals or workspace charges.

To audit elsewhere, run `sha256sum -c SHA256SUMS` from the evidence directory, extract `screen-campaign.tar.gz` into a scratch directory, and run the committed analyzer against `build/s4-results/20261023T150247Z-l40s-screen`. The archive was compared byte-for-byte against all 614 originating files, including all 86 native JSONL files, and the analyzer was rerun after extraction to a new directory with identical results apart from directory paths (see archive-verification.json). The standalone analyzer consumes the saved screen rather than executing archived absolute binary paths. Build artifact absolute paths remain provenance; rebuilding/running on another host requires fresh manifests as specified in S4_EXPERIMENTS.md. Supplementary `s4_*.py` scripts record this run's data inspection, packaging and report generation; the committed S4 runner/analyzer and untouched native records define the experiment.
'''
(root/'S4_EXPERIMENT_RESULTS.md').write_text(text)
print('Wrote',root/'S4_EXPERIMENT_RESULTS.md',len(text),'bytes')
