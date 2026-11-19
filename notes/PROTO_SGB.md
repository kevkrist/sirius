# PROTO_SGB: single-GPU / fan-in-free build for single-partition + broadcast builds (prototype notes)

Worktree `/opt/dlami/nvme/dfperf/wt-sgb`, branch `proto/single-gpu-build` (on pr2570 `ae939824`), local only.
Scripts `/opt/dlami/nvme/dfperf/runs/sgb/bin`. STATUS: final (2026-11-18). Timing plan cut short by lock starvation: see 6.4 for what did not run.

## 1. Facts established (code + existing Quent, nothing new run)

- All TPC-H forward-path Bloom sites with sources are **broadcast** builds (`4p broadcast bc`). `sirius_physical_partition.cpp:158-197`:
  the forward path pushes the SAME shared batch to all 4 slots; each slot has its own CONCAT task on its own GPU.
- Quent q3 tip (baseline_tip, hot): PARTITION(9) 4 tasks on 4 GPUs 126.8 -> 132 ms (insert 5.5 ms), Publishing Task -> 184.0 (57 ms finish).
  Then CONCAT(10): 4 tasks (one per GPU), each `input 2206 MB`, `Preparing` 184 -> 276..294 ms (92-110 ms,
  `convert:gpu_to_gpu:host_staging`), computing 3 ms; lineitem scan starts 297.5.
  => for a broadcast build EVERY GPU pulls the 3/4 of the build it does not hold: 4 x 1.65 GB = 6.6 GB of payload
  cross-GPU per q3 build, vs 2.4 GB for the Bloom finish. Every GPU ends up holding every key locally.
- Bloom transport is all-or-nothing (`find_pair_without_pool_access` over all partial spaces): HOST-staged here.
  HOST loop per chunk (<= 8 MiB): per source D2H+sync, H2D+sync, OR; root D2H+sync; 3 parallel H2D+sync
  => 2S+2 serial legs. MB4: 181 ms/GB (S=3) vs 60 ms/GB (S=0) HOST; PEER 22.5 vs 20.3 ms/GB/target (S adds ~10%).

## 2. Design-space analysis (model, from MB4 / PROFILE numbers)

Fan-in removal alone (keys reach the root for free): this machine saves (181-60) ms/GB x 0.93 GB (forward Bloom sites with
sources: q3 281, q8 174, q9 166, q21 j33 133, q5 87, q12 59, q16 57 MiB) ~ 113 ms/suite, ALL of it serial HOST legs
(bounce). P2P-equivalent: 68 -> 61 ms/GB ~ 6.5 ms/suite. So the fan-in is cheap without the bounce.

- V1 keys-to-root: TPC-H join keys are int64 (8 B/row) vs Bloom 2 B/row; with 4 even GPUs ingress = 6N bytes either way
  (no byte gain); all inserts serialize on the root (q3 22 ms instead of 5.5 ms parallel). Model: this machine ~ -5..-10 ms q3,
  P2P-equivalent a LOSS (+16 ms insert vs -2 ms fan-in). Not prototyped.
- V2 route forward tasks / batches to one GPU: for broadcast it adds payload moves (15 vs 12 batch-hops, all through one
  link). Rejected.
- V2' (chosen) local replica build where the data lands anyway: after the broadcast CONCAT's `Preparing` each GPU holds the
  whole build; each GPU inserts all keys into its own array => ZERO cross-GPU filter bytes, no merge, no scratch, no fan-out.
  Cost: each GPU inserts N keys instead of N/G (q3 22 ms vs 5.5 ms, parallel on 4 GPUs).
  Model q3: critical path 5.5 + 57 = 62.5 ms -> 22 ms here (-40 ms); P2P-eq 5.5 + 19 -> 22 (-2.5 ms).
  Construction work leaves the PARTITION task (session/plan/leases/claims stay there): it is the base site, but with
  per-GPU rebuild instead of build-once + replicate.
- V3 root = most rows / lazy leases: sites are evenly spread (4 batches on 4 GPUs) or single batch (S=0 already): no lever.
- V4 sparse partials: partial density 12.5% of bits; no cheap GPU codec; fan-in is ~10% P2P-eq anyway. Rejected.

## 3. Key widths (parquet physical types, checked; the Bloom "rep" width was not checked separately)

`o_orderkey`, `l_orderkey` INT64; `o_custkey`, `c_custkey`, `l_partkey`, `l_suppkey`, `ps_partkey`, `ps_suppkey`, `p_partkey`,
`s_suppkey`, nation keys INT32. Big forward Bloom sites q3 j15, q8 j41, q5 j32, q12 j8 are orderkey (8 B/row): V1 ingress
bytes = Bloom ingress bytes (6N for 4 even GPUs). Only q9 j29 / q16 j11 (INT32) would halve ingress. V1 not prototyped.

## 4. Prototype: local Bloom replicas for broadcast builds (`SIRIUS_PROTO_SGB=1`; off = tip behaviour)

Commit `c13359e5` (+ MB8 bench on top of cherry-picked `mb/lifecycle`). What it does, for a construction that is Bloom-only
AND whose build PARTITION is in broadcast mode with slots == replica GPUs:
- PARTITION task (`contribute`): claims its batch in the session exactly as before, but enqueues NO GPU work. The task that
  completes the inventory moves the session to PUBLISHING with `local_await` (no pending publication, `publish_if_final` no-op).
- Leases at `begin` unchanged (G x B, zero-filled).
- Each broadcast slot's CONCAT output (the whole build, one batch, already on that slot's GPU) reaches
  `sirius_physical_hash_join::push_data_batch_partitioned("build")` (re-added override, the base's one-shot hook site):
  `session.contribute_replica` inserts ALL keys of that batch into that GPU's own array (one kernel on the partial's stream,
  host wait), then deposits. Guard: delivery rows must equal the inventory's total rows, else the attempt is skipped.
- The slot that completes the last replica runs `state::publish(local)`: `accumulated_bloom_builder::finish_local` = sync
  partial streams + `make_filters`; no merge, no scratch, no cross-GPU copy, no HOST staging. Then normal fan-out to channels.
- Exact kinds / zone maps / hash-path / non-broadcast builds: untouched (tip path).
Prototype shortcuts (not production): no cancel/finalize handling of a session left in `local_await`; PARTITION's
`D_ASSERT(session.terminal())` at finalize would fire in a debug build; stats/doc/GLOSSARY not updated; exact kinds not covered;
a slot delivery in >1 batch skips the filter instead of accumulating.

Files changed -> owning PR:
- #2569 (accumulators / construction): `src/cuda/sirius_dynamic_bloom_filter.cu`, `detail/accumulated_bloom_builder.hpp`
  (`enqueue_add_local`, `finish_local`); `build_filter_construction.{hpp,cpp}` (`bloom_only`, `contribute_local`, `finish(local)`).
- #2570 (session / observer / call sites): `dynamic_filter_publisher.{hpp,cpp}` (`note_broadcast_slots`, `contribute_replica`,
  `awaits_replicas`, local mode in `contribute`/`publish`); `partition/partition_input_observer.hpp`,
  `hash_join_partition_observer.{hpp,cpp}`; `sirius_physical_partition.cpp` (note broadcast); `sirius_physical_hash_join.{hpp,cpp}`
  (build-push override).

## 5. Microbench MB8 (builder level; one thread per GPU; median of 5 after 1 warm-up; 1 process per GPU set; INT64 keys, 16 bits/key)

`tip` = each source GPU inserts its share in parallel, then `finish` on GPU 0. `local` = every GPU inserts all keys in parallel,
`finish_local`. ms. Raw: `$R/runs/sgb/mb/{g2,g4}.log`.

| Bloom MiB (keys) | G=2 PEER src=1: tip (ins+fin) / local | G=2 PEER src=2 | G=4 HOST src=1 | G=4 HOST src=2 | G=4 HOST src=4 |
|---|---|---|---|---|---|
| 57 (30M) | 1.81 (0.66+1.14) / 0.70 | 1.88 (0.39+1.49) / 0.68 | 4.12 (0.67+3.46) / 0.78 | 6.23 / 0.75 | 10.8 (0.28+10.5) / 0.73 |
| 174 (91M) | 14.2 (10.7+3.4) / 10.7 | 9.5 (5.4+4.1) / 10.7 | 21.2 (10.7+10.4) / 10.8 | 23.0 / 10.8 | 34.5 (2.8+31.7) / 10.8 |
| 281 (147M) | 26.7 (21.2+5.5) / 21.2 | 17.0 (10.6+6.4) / 21.2 | 38.0 (21.2+16.8) / 21.2 | 39.1 / 21.2 | 56.6 (5.4+51.2) / 21.2 |
| 1434 (752M) | 155 (127+28) / 127 | 95 (63.5+31.6) / 127 | 213 (127+85.6) / 127 | 209 / 127 | 292 (31.8+260.5) / 127 |

- This machine (G=4, HOST, batches on 4 GPUs = the TPC-H shape): local replicas save 10 / 24 / 35 / 165 ms at 57 / 174 / 281 /
  1434 MiB. Single-batch 57 MiB site (src=1): 4.1 -> 0.8 ms.
- Bounce excluded, measured (PEER pair, G=2, one target): local wins when the batches sit on one GPU (-1.1 / -3.5 / -5.5 / -28 ms)
  and LOSES when they are spread over both from 174 MiB up (57 MiB -1.2; 174 +1.3; 281 +4.1; 1434 +32 ms): with one target the
  copy (20 ms/GB) is cheaper than re-inserting the other GPU's half of the keys.
- Bounce excluded, G=4 (model: measured insert + MB4 PEER finish 68 ms/GB with 3 sources / 61 with 0): 281 MiB 5.4+18.7 = 24.1
  vs 21.2 (-2.9 ms); 174: 2.8+11.6 = 14.4 vs 10.8 (-3.6); 57: 4.1 vs 0.7 (-3.4); 1434: 31.8+95 = 127 vs 127 (0).
  => P2P-equivalent the two shapes are within ~3 ms per site; the local shape moves 0 filter bytes instead of (S+3) x B.
- Insert rate collapses with array size (45 G keys/s at 57 MiB, 8.5 at 174, 7.0 at 281, 5.9 at 1434): the local shape pays the
  full-array rate for all N keys on every GPU.

## 6. Engine measurements (TPC-H SF1000, 4 GPUs, this machine)

### 6.1 Quent timeline q3 join 15 (281 MiB Bloom, 147M keys, 4 batches on 4 GPUs), one hot iteration each
| | PARTITION(9) build tasks | CONCAT(10) Preparing (payload moves) | CONCAT(10) compute+sink | build site start -> first lineitem scan |
|---|---|---|---|---|
| tip (baseline_tip) | 126.8 -> 132 (insert 5.5 ms), Publishing Task -> 184.0 (57 ms) | 184 -> 276..294 (92-110 ms) | 3 ms | 170.7 ms |
| sgb (smoke, debug log) | 123.9 -> 124.0 (claims only, 0.1 ms) | 124 -> 221..233 (97-109 ms) | 24.3 ms (insert 147M keys per GPU) | 133.8 ms |
Small site q3 j8 (57 MiB, 1 batch): PARTITION(2) 4.2 -> 0.1 ms, CONCAT(3) 11.4 -> 12.1 ms.
Extension not prototyped ("SGB+"): insert each batch as it lands during `Preparing` (the local quarter at PARTITION time);
would hide most of the 21 ms on q3 under the 100 ms payload migration. Upper bound = the insert time itself (q3 21, q8 11 ms).

### 6.2 nsys, one hot iteration (prototype capture `runs/sgb/nsys_sgb`, tip capture `runs/profile/nsys_out`)
| | Bloom finish wall | Bloom cross-GPU copies inside DF ranges | insert kernels GPU-ms (all GPUs) | exact replicate |
|---|---|---|---|---|
| q3 tip | `accum::finish` 62.3 ms | PtoP 1945 MB + HtoD 942 MB + DtoH 294 MB = 3181 MB | 21.6 | - |
| q3 sgb | `finish_local` 0.1 ms | 0 | 86.6 (4 x 21.7, parallel) | - |
| q8 tip | 40.4 ms | 1274 + 607 + 182 = 2063 MB | 11.2 | 2.5 ms, 107 MB |
| q8 sgb | 0.1 ms | 0 | 45.3 (4 x 11.3) | 2.5 ms, 107 MB (unchanged) |
Leases: unchanged at `begin` (G x B zero-filled: q3 1351 MB, q8 945 MB); publication scratch + HOST staging window no longer taken.
Insert GPU work is 4x (every GPU inserts every key), but parallel across GPUs and after the payload has landed.

### 6.3 Correctness gates
- Query results: `result.txt` identical to tip for every sgb process run so far (see final table for the list).
- Filters built / published / probed (debug logs): q3 key lines identical (join 8 bloom 60007168 B, join 15 bloom 294120448 B,
  pushed=1), probes 24 chunks "1 of 1", kept 160514096 / 1927817321 rows in every hot window at tip (`runs/base/dbg_tip`) and sgb
  (`runs/sgb/smoke_sgb`): bit-identical filter effect. q8: 5 key lines identical, 8 x "1 of 1" + 50 x "1 of 2", kept
  161578660 / 6605705240 at tip (logpass) and sgb.
- `[dynamic_filter]~[!benchmark]` unit tests on the prototype binary (env off): 671 of 673 pass; the 2 failures are
  `REQUIRE(fault::available())` in `test_dynamic_filter_merge.cpp:2984,3012` (consumer host-allocation fault injection needs the
  preloaded fault library; not a prototype path). Not re-run on the tip binary: unverified that tip fails the same two.
- No unit test covers the local mode itself (prototype only exercised by q3/q5/q8/q9/q12/q16 end to end + MB8).

### 6.4 Timing (fresh process per query, 1 warm + 5 hot, first hot dropped, median of the remaining hot iterations; all under the lock)
A = tip binary `$R/repo`, B = prototype `$R/wt-sgb` with `SIRIUS_PROTO_SGB=1`. Order per query A B B A. Raw: `$R/runs/sgb/t1_*`,
table: `python3 $R/runs/sgb/bin/tab.py tip sgb t1`. The lock was heavily contended (30-85 waiters): processes of one ABBA block
are 5-60 min apart, so the interleaving does not protect against slow drift as well as usual.

| q | tip ms (n hot; process medians; sd) | sgb ms | sgb - tip | protocol |
|---|---|---|---|---|
| 3 | 662.1 (8; 662 665; sd 26 = one outlier) | 623.8 (8; 629 624; sd 6.5) | **-38.3 (-5.8%)** | full ABBA |
| 8 | 936.8 (8; 938 936; sd 1.5) | 907.3 (8; 911 902; sd 5.2) | **-29.5 (-3.2%)** | full ABBA |
| 5 | 692.1 (8; 692 692; sd 14) | 673.8 (8; 675 674; sd 14) | **-18.3 (-2.6%)** | full ABBA |
| 9 | 1415.4 (8; 1425 1410; sd 15) | 1383.1 (8; 1383 1383; sd 11) | **-32.4 (-2.3%)** | full ABBA |
| 12 | 700.3 (4; sd 3.0) | 692.3 (4; sd 2.1) | -8.0 (-1.1%) | ONE A-B pair only |
| 16 | 308.1 (4; sd 6.8) | 301.6 (4; sd 3.4) | -6.4 (-2.1%) | ONE A-B pair only; inside the +-10 ms noise band |
| sum of the six | 4715 | 4582 | **-133 ms** (= -0.86% of the 15372 ms suite) | |

Deltas match MB8 (G=4 HOST, src=4) per site: q3 35.4 + 3.3 = 38.7 predicted vs 38.3; q8 23.7 + 3.3 = 27 vs 29.5.
Every gain here is HOST-staged finish time that no longer exists (bounce time, "this machine" column).

NOT RUN (lock starvation; jobs stopped while still waiting, no partial process): controls q21, q7, q10, q2, q11, q6; the second
half of ABBA for q12 / q16; the direct base-vs-sgb ABBA; the full 22-query suite. By construction the local mode cannot trigger on
the hash path (needs the forward path's broadcast note) nor on exact-only plans, and with the env off the only added code on any
path is one mutex-protected bool read per build delivery; this is an argument, not a measurement.

### 6.5 Against base (tip - base from BASE_REFERENCE section 3 / ABBA confirm blocks; NOT re-measured here)
| q | tip - base (earlier campaign) | sgb - tip (this run) | gap closed? |
|---|---|---|---|
| 3 | +31..35 | -38.3 | yes (sgb 624 vs base 631 in the earlier campaign) |
| 8 | +35..36 | -29.5 | ~85% (907 vs 904) |
| 5 | +11..15 | -18.3 | yes (674 vs 682) |
| 16 | +8 | -6.4 (weak) | ~80%, unresolved |
| 9 | +4..9 | -32.4 | beyond base (1383 vs 1398) |
| 12 | -7 | -8.0 (weak) | n/a |
Suite: measured -133 ms on six queries. Unmeasured forward Bloom sites of other queries (model from MB8: q21 j33 133 MiB ~ -20 ms,
q22 j28 ~ -6 ms, q20 / q7 small sites ~ -3 ms each) put the suite estimate at about -150..-165 ms (~1.0%), i.e. the 147 ms base
gap is closed or nearly closed on this machine. That suite figure is an estimate; no suite pass was run.

### 6.6 Bounce excluded (P2P-equivalent)
- Measured only on the PEER pair (G=2, MB8): see section 5. With one target the local shape loses on spread builds >= 174 MiB.
- G=4 P2P model (measured insert + MB4 PEER rates): -3 to -3.6 ms per Bloom site up to 281 MiB, 0 at 1.4 GiB.
  q3 ~ -6 ms, q8 ~ -7 ms, q5 / q9 / q12 / q16 ~ -3..-7 ms each: suite roughly -25..-40 ms (0.15-0.25%), below the 0.5% suite gate.
  Cross-GPU filter bytes still go from (S+3) x B to 0 (q3: 2.4 GB of Bloom payload -> 0), and the publication scratch disappears.
- Honest reading: without the bounce, fan-in costs ~10% of a finish that is link-bound anyway; re-inserting on every GPU costs
  about what the fan-out copy costs. The big number on this machine is the serial HOST-staged loop (2S+2 legs per 8 MiB chunk).

## 7. Interaction with OP1 (finish off the Publishing Task)

- Same sites, alternative mechanisms: on broadcast Bloom sites SGB leaves no finish to defer (0 bytes, `finish_local` 0.1 ms).
  What remains on the critical path is the per-GPU insert at the CONCAT sink (q3 21 ms, q8 11 ms); OP1's continuation idea
  (or SGB+: insert per batch as it lands in `Preparing`) could hide that too. Not additive on these sites: SGB subsumes OP1's
  gain there, and without adding 2.4 GB (q3) of Bloom traffic to the links / pinned-host path while CONCAT moves 6.6 GB of
  payload (OP1's unmeasured contention risk).
- Additive across site classes: SGB cannot apply to hash-partitioned builds (each GPU holds only its partitions): q21 j18,
  q7 j35, q10, q4 still need OP1. Exact kinds on broadcast sites could use the same local build (drops `replicate_to_devices`,
  ~18 ms/suite by PROFILE) but were not prototyped.
- If OP1 lands first, SGB's this-machine gain shrinks to whatever OP1 could not hide in the slack, plus the removed contention.

## 8. Complexity / risk / recommendation

- Size: ~250 lines over 11 files; the production version needs a second certified "replica contribution" path in the session
  (per-GPU completeness, retry identity per slot, cancel/finalize while awaiting replicas, stats, estimates for the CONCAT-side
  insert, a slot delivered in several batches, exact kinds). Medium complexity, mostly session state (#2570).
- Architectural cost: construction WORK moves back to the CONCAT -> join deposit (the base's site) for broadcast builds; only
  plan / leases / claims stay at the PARTITION Construction Site. Publication happens ~100 ms later than at tip (after the
  payload migration), still before the first probe scan in every hot window checked (q3 24/24 chunks probed, q8 58/58 as tip);
  the scheduler creates the probe scans after the build CONCAT (PROFILE 1.2), so nothing is lost on this suite.
- Risks: 4x insert GPU work (q21-size arrays would cost 127 ms per GPU: only worth it under HOST staging; needs a size / topology
  policy); loses on 2-GPU P2P machines for spread builds >= 174 MiB (+1..+32 ms); memory unchanged (G x B leased from `begin`).
- Recommendation: (1) do NOT pursue V1 keys-to-root or V2 routing (analysis: no byte gain for int64 keys / more payload hops).
  (2) Local replicas are the measured optimum for broadcast Bloom builds on this machine (-133 ms on six queries, results and
  filter effect identical) and are the base shape made cheaper (rebuild per GPU instead of build once + replicate). Because the
  bounce-excluded gain is only ~0.2% (model), adopt it only as a transport-aware policy: use local replicas when the Bloom
  transport would be HOST-staged (the builder already knows: `find_pair_without_pool_access`) or when the build is single-batch;
  keep the PARTITION-site merge on full-P2P machines. (3) Before any review: run the controls and a full suite pass (not done),
  and decide OP1 vs SGB on the broadcast sites with both prototypes measured in one ABBA.

## 9. Reproduce
```bash
R=/opt/dlami/nvme/dfperf; B=$R/runs/sgb/bin; L=$R/machine.lock
flock $L $B/build.sh                                   # pixi run make in wt-sgb
$B/abba.sh t1 tip sgb 3 8 5 16 9 12                    # A B B A per query, lock per process
python3 $B/tab.py tip sgb t1; $B/resdiff.sh 3 5 8 9 12 16
flock $L $B/mb.sh g2 0,1 "[mb8]" SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_REPS=5   # PEER pair
flock $L $B/mb.sh g4 0,1,2,3 "[mb8]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5
EXTRA="--precmd nsys --query-timeout 3600" flock $L $B/oneq.sh sgb nsys_sgb 3 3
python3 $B/probe.py <bench dir>                        # probes ran / kept rows / key lines per window (debug log)
python3 $B/tl.py <quent session dir> 2 0.0             # task timeline of one hot query
```
Telemetry: `$R/telemetry/sgb/<label>/q<N>/`. Commits (local only): `c13359e5` (prototype; has a declaration-order compile
error fixed in the next commit), `0ee6d8ae` (cherry-picked lifecycle bench), `d35151cf` (fix + MB8).
