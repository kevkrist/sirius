# VERIFY_PROTO_SGB: adversarial verification of PROTO_SGB (local Bloom replicas for broadcast builds)

Verifier, 2026-11-19. Prototype untouched (`$R/wt-sgb`, `d35151cf`, env `SIRIUS_PROTO_SGB=1`). Tip = `$R/repo` (`ae939824`), base = `$R/wt-base`.
Scripts `$R/runs/vsgb/bin`, raw `$R/runs/vsgb`, Quent `$R/telemetry/vsgb/<label>/q<N>/`. Every GPU process under `flock $R/machine.lock`.
Config `/home/ubuntu/sirius/specific_config.yaml` unmodified. Nothing pushed.

## 0. Verdict

**Timing claim on this machine: CONFIRMED** (and extended: controls, base, q21/q22 now measured).
**Correctness on TPC-H: CONFIRMED** (results, filters, probes, operator row counts identical; ordering holds in every hot window).
**As an optimization of the DF stack: WEAKENED**, for three reasons that are not timing noise:
1. The gain is HOST-bounce time (210 ms of HOST-staged publishing task replaced by 45 ms of insert). P2P-equivalent it is about
   -50 ms / suite (0.3%, model on measured PEER rates), under the 0.5% gate; on a 2-GPU P2P machine the shape loses on spread builds >= 174 MiB (measured, both PEER pairs).
2. It publishes at the END of the build CONCAT, 5-73 ms later than tip publishes (measured per site, section 4). That is the opposite
   of the stack's stated goal (S6 eager probe scans: filter ready at PARTITION, scan starts before the build CONCAT). Under S6 it
   forfeits ~280 ms of filter head start on the six claim queries to gain ~125 ms of today's schedule.
3. The prototype has a session state that never terminates (section 6), plus the production gaps the author lists.

## 1. Timing (fresh process per query, 1 warm + 5 hot, first hot dropped, 4 hot x 2 processes per arm, one lock hold per query block)

Block v1, 02:37-03:06 UTC, back to back (no drift gap between arms; each process ~23 s). ms, pooled median (process medians) sd.

| q | order | tip | sgb | sgb - tip | prototype claimed |
|---|---|---|---|---|---|
| 3 | tip sgb base base sgb tip | 661.9 (662 661) sd 25 (one 738 outlier) | 623.1 (621 626) sd 6 | **-38.8 (-5.9%)** | -38.3 |
| 8 | sgb tip base base tip sgb | 936.0 (934 936) sd 3 | 909.4 (909 910) sd 3 | **-26.6 (-2.8%)** | -29.5 |
| 5 | tip sgb sgb tip | 692.5 (693 692) sd 14 | 674.9 (675 675) sd 15 | **-17.6 (-2.5%)** | -18.3 |
| 9 | sgb tip tip sgb | 1415.3 (1412 1425) sd 15 | 1389.0 (1377 1405) sd 17 | **-26.3 (-1.9%)** | -32.4 |
| 12 | sgb tip tip sgb | 702.5 (702 704) sd 2 | 693.2 (693 693) sd 2 | **-9.3 (-1.3%)** | -8.0 (1 pair) |
| 16 | tip sgb sgb tip | 313.2 (313 314) sd 3 | 306.4 (305 308) sd 3 | **-6.8 (-2.2%)** | -6.4 (1 pair) |
| sum of the six | | 4721 | 4596 | **-125.4** | -133 |
| 21 (j33 forward 133 MiB + j18 hash) | tip sgb sgb tip | 1442.4 (1443 1438) sd 14 | 1423.4 (1423 1427) sd 12 | -19.0 (-1.3%); -15.5 by process medians | not run (model -20) |
| 22 (j28 forward) | sgb tip tip sgb | 338.8 (339 339) sd 0.6 | 331.4 (331 332) sd 0.9 | -7.4 (-2.2%) | not run (model -6) |
| 7 (hash-path control) | sgb tip tip sgb | 747.2 (747 747) sd 2 | 751.1 (751 751) sd 4 | +3.9 (+0.5%) | not run |
| 10 (hash-path control) | tip sgb sgb tip | 847.2 (858 843) sd 8 | 845.4 (855 838) sd 10 | -1.8 (-0.2%) | not run |
| all ten | | 8097 | 7947 | **-149.7 (-0.97% of the 15372 ms suite)** | est. -150..-165 |

- q21 is NOT a pure control: its join 33 is a forward Bloom site and goes local (log pass). q7 and q10 are pure controls
  (0 local publications): q10 flat; q7 +3.9 ms with tight sd on both pairs (see section 1.2).
- The per-site Quent timelines explain every delta (section 4): PARTITION publishing task disappears, build CONCAT grows by the
  full-array insert.

### 1.1 Against base (same lock hold)

| q | tip - base | sgb - base | gap closed |
|---|---|---|---|
| 3 | +42.0 (base 619.9, sd 14) | **+3.2** | 92% (parity within noise) |
| 8 | +35.1 (base 900.8, sd 3) | **+8.5** | 76% (claimed ~85%); 8.5 ms residual is real at sd 3 |

q5 / q9 / q16 vs base not re-measured. The "147 ms base gap closed" statement holds for q3; for q8 a ~8 ms residual remains (the
per-GPU re-insert of 91M keys, 11.3 ms, which base does once).

### 1.2 Second wave (v2 / c1 / c2 blocks)

SECOND_WAVE_PLACEHOLDER

## 2. Correctness

- **Query results**: `result.txt` identical to the tip baseline in every process: sgb 3 processes each for q3 q5 q8 q9 q12 q16 q21 q7 q10
  (2 timing + 1 trace), 1 each for q22 q20 q4; tip and base arms also identical (`resdiff.sh`).
- **Log pass diff** (`lp_cmp.py`, tip TRACE pass `runs/baseline/logpass` vs prototype TRACE pass `runs/vsgb/lp_sgb`, hot window, q3 q5 q8
  q9 q12 q16 q21 q22 q7 q20 q10 q4): identical in all 12 queries for: `[dynamic_filter_key]` lines (join, rows, valid, kind, size,
  zone_map, pushed, reason), constructions started, publications (filters x targets), Bloom bytes per finish line, compressed-scan
  `join filters kept X/Y (N of M probes ran)` histograms and row sums, and **rows produced by every operator id** (18-59 operators,
  57-387 tasks per query). Bloom arrays are deterministic, so identical operator rows = bit-identical filter effect and no scan that
  missed a filter.
  Local publications in the hot window: q3 2, q5 2, q8 2, q9 2 (incl. the two-key join 29), q12 1, q16 1, q21 1, q22 1, q20 1;
  q7, q10, q4 0 (hash path and BUILD_FILTER untouched, as claimed). IN-list kinds on broadcast sites stay on the tip path.
- **No consumer scan before its filter is visible** (`quent_order.py` on my timing telemetry, 8 hot windows per query, q3 q5 q8 q9 q12
  q16 q21 q22): the first consumer scan task is created 0.0-0.2 ms after the last build CONCAT task ends, at tip and at sgb; 0 windows
  with a consumer task before that. The local publication runs synchronously inside the last slot's CONCAT sink, before the deposit,
  so the "immediate BUILD_PROBE probe" edge of the doc still orders it. It is the only edge: a transitive / lookahead target gets the
  filter 5-73 ms later than at tip (none exists on these sites in TPC-H, so not observable here).
- **Unit tests** `[dynamic_filter]~[!benchmark]`: tip binary 671/673, prototype env off 671/673, prototype env on 671/673. The same two
  fail in all three: `REQUIRE(fault::available())` at `test_dynamic_filter_merge.cpp:2984, 3012` (fault library not preloaded). So the
  author's "unverified that tip fails the same two" is now verified. `[partition],[operator],[routing],[exchange]` (220 cases): pass
  on tip and on prototype env on. No unit test exercises the local mode (0 "PROTO local" lines in the env-on logs).
- **Code review** (edge cases asked for):
  - zero-fill vs local insert: no race (both on the partial's own stream); same kernel, same null handling, nothing allocated.
  - batch not on the GPU tier at the CONCAT push: attempt ends SKIPPED (filter lost, no wrong result). Same policy as tip's
    contribution, evaluated ~100 ms later.
  - OOM on the root: there is no root work left (no scratch, no staging). Strictly more robust than tip here.
  - multi-key joins: covered (q9 j29, two 83 MiB arrays), identical rows.
  - exact kinds / zone maps: `bloom_only()` gate sends them to the tip path (q8, q20, q7, q2-style sites unchanged in the logs).
  - "multi-GPU heavier on a non-root": no root in local mode; cost is uniform (every GPU inserts everything).
  - slot delivered in several batches: unreachable today (broadcast implies BUILD_PROBE implies `concat_all`), the row-count guard
    would skip the filter.
  - two slots delivering on the same GPU (task re-placed after an OOM retry), a slot that never delivers, query cancel while
    waiting: session stuck, see section 6.

## 3. Bounce vs non-bounce

### 3.1 Bytes (independent whole-window accounting, `nsys_bytes.py` on the existing captures: all memcpy in the hot `sirius::query` window)

| | tip MB (PtoP + HtoD + DtoH) | sgb MB | delta | insert kernels per GPU | `or_bloom_chunk` | DF probe kernels |
|---|---|---|---|---|---|---|
| q3 | 57755 | 54574 | **-3181** (= claim) | 5.4 -> 21.7 ms on each of 4 | 108 -> 0 | 104 = 104, same ms |
| q8 | 87136 | 85071 | **-2065** (claim 2063) | (3.5 2.0 2.3 3.4) -> 11.3 x 4 | 66 -> 0 | 117 = 117 |

Nothing moved elsewhere in the window. NVTX: `accum::finish` 62.4 / 40.4 ms -> `finish_local` 0.07 ms; `contribute_local` 4 x 21.7 / 11.3 ms.

### 3.2 PEER microbench MB8, re-run (prototype binary, 5 reps, ms; `runs/vsgb/mb/*.log`); reproduces the author's table within 1%

| Bloom MiB | G=2 PEER (0,1) src=2: tip / local | G=2 PEER (2,3) src=2 | G=2 HOST pair (0,2) src=2 (new) | G=4 HOST src=4 |
|---|---|---|---|---|
| 57 | 1.87 / 0.67 | 1.87 / 0.68 | 5.08 / 0.67 | 10.8 / 0.74 |
| 174 | 9.44 / 10.77 | 9.46 / 10.75 | 19.6 / 10.8 | 34.5 / 10.8 |
| 281 | 17.04 / 21.21 | 17.02 / 21.15 | 33.5 / 21.2 | 56.5 / 21.2 |
| 1434 | 95.4 / 127.3 | 95.1 / 127.0 | 180.4 / 127.3 | 292.0 / 127.2 |

- Same two GPUs, same bytes: PEER tip 17.0 ms vs HOST tip 33.5 ms at 281 MiB. The prototype beats tip only where the transport is HOST.
- PEER, spread build: local loses by +1.3 (174), +4.2 (281), +32 ms (1434); wins by 1.2 ms at 57 MiB. Single-source PEER: local wins
  by the fan-out copy (1.1 / 3.4 / 5.5 / 28 ms).

### 3.3 Split of the measured gain

Per site (Quent task durations, my v1 runs, medians of 4 hot windows): gain = PARTITION publishing task removed (share insert + finish)
minus build CONCAT growth (full-array insert). P2P-equivalent PARTITION task = share insert + 3 x fan-out(B) + S x fan-in(B) with the
measured PEER rates of 3.2 (fan-out 1.14 / 3.43 / 5.54 / 28.1 ms at 57 / 174 / 281 / 1434 MiB per target; fan-in +0.35 / 0.6 / 0.9 / 3.5 ms
per source), i.e. one root link carrying the three targets one after the other.

| q | site | PART task removed (this machine) | CONCAT growth | predicted | measured (per query) | PART task P2P-equivalent (model) | P2P-equivalent gain (model) |
|---|---|---|---|---|---|---|---|
| 3 | j15 281 MiB, 4 src | 57.3 | +18.5 | -38.8 | | 24.6 | -6.1 |
| 3 | j8 57 MiB, 1 src | 4.2 | +0.4 | -3.8 | **-38.8** | 4.1 | -3.7 |
| 8 | j41 174 MiB | 35.6 | +13.0 | -22.6 | | 15.6 | -2.6 |
| 8 | j34 57 MiB, 1 src | 4.1 | +0.9 | -3.2 | **-26.6** | 4.1 | -3.2 |
| 5 | j32 87 MiB | 16.8 | +2.6 | -14.2 | | 6.9 | -4.3 |
| 5 | j25 57 MiB, 1 src | 4.2 | -0.4 | -4.6 | **-17.6** | 4.1 | -4.5 |
| 9 | j29 2 x 83 MiB | 32.0 | +3.0 | -29.0 | | 12.7 | -9.7 |
| 9 | j19 21 MiB | 3.6 | +0.5 | -3.1 | **-26.3** (sd 15) | 1.7 | -1.2 |
| 12 | j8 59 MiB, 44 batches | 10.9 (1.2 of PARTITION work stays) | +0.7 | -10.2 | **-9.3** | 4.7 | -4.0 |
| 16 | j11 57 MiB | 8.3 | -0.7 | -9.0 | **-6.8** | 4.1 | ~ -3 |
| 21 | j33 133 MiB | 26.2 | +5.4 | -20.8 | **-19.0** | 11.4 | -6.0 |
| 22 | j28 | 7.3 | +0.8 | -6.5 | **-7.4** | ~3.5 | ~ -2.7 |
| sum | | 210.5 | +44.7 | -166 | **-152** | ~98 | **~ -50 (0.33% of the suite)** |

- **This machine: -152 ms on the eight affected queries (-150 ms net of the two controls, -0.97% of the suite)**, measured. All of it is
  time the HOST-staged finish spent on the Publishing Task (210 ms of PARTITION task replaced by 45 ms of CONCAT growth).
- **Bounce excluded / full-P2P 4-GPU machine: about -50 ms (0.3%)**, model only (3 PEER targets cannot be measured here). About 11 ms of it
  is the three single-batch 57 MiB sites, where HOST fan-out is already as fast as the PEER model (4.1 vs 4.1 ms): there the saving is
  "re-insert 30M keys (0.7 ms) instead of copying 57 MiB three times", not bounce. The model assumes one root link serving three
  targets serially; if three PEER targets ran in parallel, tip would finish 281 MiB in ~6.4 ms and the local shape would LOSE ~7 ms on
  q3 j15. Either way: under the 0.5% suite gate, 3-10 ms per query (inside the +-10 ms noise band), not a measurable win on a full-P2P
  machine. The author's "-25..-40 ms, below the gate" stands (my model is slightly higher, same conclusion).
- 2-GPU P2P machine: measured loss on spread builds >= 174 MiB (+1.3 / +4.2 / +32 ms), measured win only on single-source builds.

## 4. Publication timing: the conflict with eager probe scans (S6)

Quent, medians of 4 hot windows, ms from query start (my v1 runs). "ready" = when the filter is visible: tip = end of the PARTITION
publishing task; sgb = end of the last build CONCAT task. First consumer scan is created at CONCAT end + 0.1 ms in both today.

| site | tip PART start-end | tip CONCAT end | sgb PART end | sgb CONCAT end | filter ready: sgb - tip | first scan: sgb - tip (today's schedule) |
|---|---|---|---|---|---|---|
| q3 j15 | 124.4-181.7 | 291.9 | 120.6 | 249.3 | **+68** | -42.7 |
| q8 j41 | 161.2-196.8 | 296.1 | 157.8 | 270.1 | **+73** | -26.2 |
| q5 j32 | 181.8-198.6 | 254.9 | 178.3 | 237.2 | **+39** | -17.7 |
| q9 j29 | 406.4-438.4 | 489.2 | 399.9 | 453.7 | **+15** | -35.5 |
| q12 j8 | 612.2-624.3 | 655.5 | 613.9 | 645.8 | **+22** | -9.5 |
| q16 j11 | 55.3-63.7 | 137.0 | 55.5 | 128.1 | **+64** | -8.9 |
| q21 j33 | 897.1-923.3 | 950.7 | 895.1 | 927.9 | +5 | -22.8 |
| q22 j28 | 139.1-146.4 | 150.3 | 139.0 | 143.7 | -3 | -6.6 |

The gain exists only because today's scheduler starts the probe scan after the build CONCAT anyway: the prototype trades a 57 ms
finish before CONCAT for a 16 ms insert inside it. With the user's S6 decision (probe scans scheduled as soon as the filter is ready,
before the build CONCAT) tip's filters are ready 15-73 ms earlier than the prototype's on six of eight sites (sum 281 ms on the six
claim queries); a local replica can never be ready before the payload has migrated to every GPU. So this prototype and S6 are
alternatives on broadcast sites, and S6 is the stated goal. OPPORTUNITIES.md (user decisions block) already demotes "local replicas
after CONCAT" for this reason; the measurements above quantify it.

## 5. Memory

- Leases: identical. `construction::begin` is unchanged (G x B, zero-filled); nsys memset bytes per hot window identical (q3 1461.8 MB,
  q8 1040.2 MB). Held for the same span (begin -> join end).
- Task reservations (Quent `Reserving.requested_bytes`): identical for PARTITION and CONCAT tasks (q3 PARTITION(9) 1025 / 1196 MB,
  CONCAT(10) 3839 MB). `task_scratch_bound()` is still charged to every build PARTITION task in local mode although nothing is taken.
- Task peak allocated (TRACE `peak allocated`, max per operator kind): CONCAT identical in all 12 queries; PARTITION identical except
  the publication scratch that is no longer taken: q5 8 -> 0 MB, q12 8 -> 0 MB (other queries' PARTITION peaks are set by other tasks).
- Pool peak (`[gpu_pool] QueryEnd peak=`) is a process-lifetime high-water mark, byte-identical tip vs sgb; not informative.
- Net: no per-GPU peak change; the HOST staging window and <= 8 MiB-per-source scratch are gone. No memory argument for or against.

## 6. Issues a production implementation must solve

1. **Non-terminating session.** After the last PARTITION claim the session is PUBLISHING with `local_await` and no pending
   publication. `finish_input()`, `cancel()` and the destructor only resolve OPEN / COLLECTING / a pending publication, so if any
   replica GPU never delivers (query cancelled or failed between PARTITION and the build CONCAT; a slot's CONCAT re-placed on a GPU
   that already delivered - the per-device dedup then drops it; a LIMIT-style early stop) the session never becomes TERMINAL:
   leases (G x B, unspillable) live until the join operator is destroyed, no key line / stats, publication rights never resolve.
   Harmless for results today (consumers never wait); a hang under S6's "a scan waits for all its filters". The doc invariant "every
   enabled session is Terminal by Build Layout Completion" is broken by design (the author notes the debug `D_ASSERT`).
2. **Completeness is by row count only** (`delivery rows == inventory rows`), per device, with no certificate that the delivery is the
   certified build (the PARTITION-site ledger certifies IDs, carriers and null counts). Needs a per-slot identity, retry identity, and
   the carrier / null-count check the `reserve` path does.
3. **Later publication** (section 4): conflicts with S6; and any transitive / lookahead target sees the filter up to ~70 ms later.
4. **Transport-aware policy is mandatory**: local loses on PEER for spread builds >= 174 MiB (measured) and costs G x the insert GPU
   work (q3 21.6 -> 86.6 GPU-ms; a q21-j18-sized array would be 127 ms per GPU). Needs size and topology gates, and an estimate for
   the CONCAT-side insert in the CONCAT task's admission.
5. Construction work in the CONCAT sink of another operator (host wait inside `push_data_batch_partitioned`); stats, exact kinds,
   multi-batch deliveries, docs / glossary: not done (author's list, confirmed).
6. Reservation: drop the unused `task_scratch_bound()` charge on local-mode PARTITION tasks.
7. Tests: none covers the mode.

## 7. What ran / what did not

Ran (all under the lock): v1 ABBA blocks q3 q8 (with base) q5 q9 q21 q7 q10 q12 q16 q22 (2 processes per arm, 4 hot each);
prototype TRACE pass on 12 queries; unit tests x5 runs; MB8 on 4 GPU sets. Second wave: see 1.2.
Offline: nsys re-analysis of the author's captures (not re-captured), Quent ordering, code review.
Not run: full 22-query suite ABBA (the ten queries above carry every site the mode can touch plus two controls; the other 12 have
no local publication by construction, see 1.2 for the ones checked); base vs sgb on q5 / q9 / q16; engine runs on a 2-GPU PEER
configuration (the config cannot be changed and the prototype has no GPU-count override); OP1 vs SGB head to head; debug build.

## 8. Reproduce
```bash
R=/opt/dlami/nvme/dfperf; B=$R/runs/vsgb/bin; L=$R/machine.lock
flock $L $B/arms.sh v1 6 3 tip sgb base base sgb tip      # one lock hold per query block
python3 $B/tab.py tip sgb v1; python3 $B/tab.py base sgb v1; python3 $B/raw.py v1
flock $L $B/logpass.sh sgb lp_sgb 3 5 8 9 12 16 21 22 7 20 10 4
python3 $B/lp_cmp.py $R/runs/baseline/logpass $R/runs/vsgb/lp_sgb 3 5 8 9 12 16 21 22 7 20 10 4; $B/resdiff.sh 3 5 8 9
python3 $B/quent_order.py $R/telemetry/vsgb/v1_sgb_1
flock $L $B/unit.sh; flock $L $B/mball.sh
python3 $B/nsys_bytes.py <nsys.sqlite>
```
