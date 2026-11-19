# PROTO_OP2: filters that are built but have no consumer effect (OP2, + OP3 oracle)

2026-11-18. Worktree `$R/wt-op2`, branch `proto/op2-dead-filters` (base pr2570 `ae939824`), 3 local commits, nothing pushed.
Scripts `$R/runs/op2/bin/` (one_query.sh, pal.sh, pal_table.py, gate_summary.sh, unit.sh, build.sh). Raw output `$R/runs/op2/`,
Quent `$R/telemetry/op2_*`. Every build / GPU process ran under `flock $R/machine.lock`. Config untouched; prototype switches
are env vars read once (`src/op/dynamic_filter/proto_op2.hpp`), all off by default.

## 0. Bottom line

1. **q8 j41 and q21 j18 are the same case: the Bloom is built, merged, replicated and then skipped by policy on every batch.**
   Two pre-existing consumer rules do it, not a wiring bug and not a gate verdict:
   - decode carries at most one join probe (`fused_scan_max_membership_sources = 1`, the strongest) -> the Bloom is the dropped tail;
   - the endpoint drops a filter whose estimated removed share of the batch is < 10% once stronger filters are counted
     (`k_negligible_removed_share`, `dynamic_filter_merge.cpp`, from #2585): q8 0.0067 x (1 - 0.061) = 0.6%; q21 0.04 x (1 - 0.487) = 2.1%.
   Making the filter "useful" (apply it at the endpoint after the decode applied the stronger one) gains nothing (q8 +0.4 ms).
   So the fitting fix is: do not build it.
2. **Shadow rule at construction begin** (prototype, `SIRIUS_PROTO_OP2_SHADOW=1`): skip a key when every one of its bindings
   would be dropped by the endpoint's own rule, judged against the filters already published on the target channel, using only
   estimates from keys with a uniqueness proof / declaration. It fires on exactly 2 of 46 constructions: q8 j41 (174 MiB) and
   q21 j18 (1394 MiB, the largest construction of the suite). Results identical.
3. **q9 j29 is a different case: the useless key (ps_suppkey, keeps 98.9%) poisons the useful one.** Decode picks the suppkey
   Bloom as its single probe, the first chunks decline compaction as unselective, and that sets the scan-wide sticky latch
   `pushdown_selection_unprofitable`; the useful partkey Bloom (keeps 5.4%) is then never applied during decode on any chunk,
   only post-decode. No plan-time rule can catch it without a distinct count (static estimate says suppkey 0.054, partkey 0.217:
   both wrong, and in the wrong order). Prototype `SIRIUS_PROTO_OP2_LATCH=1` (consumer, pre-existing code): do not latch when
   every probe the decode carried was itself weak.
4. OP3 (distinct-aware sizing, oracle): no measurable gain on top (see 3.2). Drop.

## 1. Step 0: diagnosis (DEBUG logging, commit `d35058cb`)

New DEBUG lines `[df_gate]` (consumer; `dynamic_filter_merge.cpp`, `sirius_gpu_scan_operator_data.cpp`):
- `decode ch= gen= bound= applicable= needs_combined= decode_trains=` and per filter `decode ... filter=[kind:size@id est=] keep= decision=attach|skip_measured_weak`
- `decode-probe ... rows=A->B keep=` / `decode-combined ... complete=` (what the decode measured), `decode-probe DECLINED` (never fired)
- `endpoint ch= gen= rows= candidates= applicable= ...` and per filter `decision=apply|skip reason=already_applied|no_replica_on_device|carrier_type|measured_weak|negligible_removed_share`
- `gate=... decode-combined|endpoint-combined ... state=ACTIVE|DISABLED|UNDECIDED`, `gate=... marginal filter@ keep=`
- `decode gave compaction up: latch=true|false`
`gate_summary.sh <log>` collapses them for the hot window. Runs: label `diag0` (switches off = tip behaviour), 2 iterations, debug level.

| filter | decode (cap = 1 probe) | endpoint | verdict |
|---|---|---|---|
| q8 j41 Bloom 174 MiB on l_orderkey, est 0.061 | attached on 50/50 chunks, never run: the hash IN-list of j44 (est 0.0067) takes the one slot | `skip negligible_removed_share` on 50/50 batches | policy-dead; useless even if applied (2.3) |
| q21 j18 Bloom 1394 MiB on l_orderkey (l1), est 0.487 | attached on 31/31, never run (j21 hash IN-list est 0.04 takes the slot) | `skip negligible_removed_share` 31/31 | policy-dead |
| q2 j28 hash IN-list 2.0M keys (supplier) on partsupp, est 0.20 | attached 4/4, never run (j25 part IN-list est 0.004) | `skip negligible_removed_share` 4/4 | policy-dead, 3.2 ms build, no bounce |
| q9 j29 c1 Bloom 83 MiB on l_suppkey, est 0.054 (true keep 0.989) | **chosen as the one probe** on 16 chunks, 1.57 B rows probed, decode declines (unselective) -> scan latched | measured 0.989 -> `skip measured_weak` 62/62 | genuinely useless **and harmful** |
| q9 j29 c0 Bloom 83 MiB on l_partkey, est 0.217 (true keep 0.054) | never run at decode (slot taken, then latch) | applied post-decode on 62/62 batches (6.0 B rows -> 326 M) | useful, but applied late: consumer defect |
| q20 j49 hash IN-list (est 0.040) / j18 Bloom (est 0.011) on partsupp | IN-list runs (kind rank beats estimate) | Bloom applied on the survivors; IN-list skipped as negligible (already applied) | both useful |
| q16 j11 Bloom 57 MiB on ps_partkey, keep 0.15 | probed on 2/2 chunks, decode gives compaction up (0.15 > 0.10 full-route limit), latch | applied again post-decode (800 M rows probed twice) | useful; double probe is a consumer inefficiency, not a construction issue |
| q11 (4 filters, keep 0.04 each, one per channel) | applied at decode | - | all useful; nothing to skip |

Why DF net <= 0 on q2 / q16 / q20 / q11: q2 = j28 dead IN-list (3.2 ms) + j42 skipped (targets drained); q16 = 8.5 ms finish (bounce)
+ the double probe; q20 and q11 have no dead filter (small queries, all filters effective). A construction-skip rule has nothing
to remove in q16 / q20 / q11.

Static estimate facts: `estimated_selectivity = build rows / base-scan rows of the key's origin table`
(`dynamic_filter_publisher.cpp:200`, value domain from `sirius_plan_comparison_join.cpp:547`). Exact for a unique build key
(p_partkey, o_orderkey: `eligible=1` in `[dynamic_filter_domain]`), meaningless for a non-unique one (q9 j29: ps_suppkey 43.5M rows
/ 800M partsupp rows = 0.054 while 9.89M of 10M suppliers are present; `eligible=0`).

## 2. Prototype

Commits on `proto/op2-dead-filters`: `d35058cb` logging, `6ebc0a98` switches, `523ae08c` logging free at INFO.

| switch | change | file (owner per pr_split) |
|---|---|---|
| `SIRIUS_PROTO_OP2_SHADOW=1` | In `dynamic_filter_publication_session::begin`, before `plan_construction`: for each admitted key with a known value domain **and** `domain_gate_eligible()`, own = rows/domain; for each binding, survival = product of `estimated_selectivity()` of the filters already on the target channel that are reliable and stronger; if every binding has `survival < 1 && survival x (1 - own) < 0.10` the key's bindings are dropped, so `plan_construction` passes over it (key line: `kind=none reason=not_constructed`). DEBUG `[df_proto] shadow join= condition= own_est= max_removed_share= decision=skip|build` | `dynamic_filter_publisher.cpp` (session, **#2570**); the decision belongs in `plan_construction` as a per-binding fact + a reason token (**#2569** plan policy) |
| (support) `sirius_dynamic_filter::set_estimate_reliable` | set next to `set_estimated_selectivity` from `domain_gate_eligible()` | `sirius_dynamic_filter.hpp` (pre) + `dynamic_filter_publisher.cpp` (#2570) |
| `SIRIUS_PROTO_OP2_LATCH=1` | `scan_operator_input` does not set `pushdown_selection_unprofitable` when the declined decode carried an incomplete join-filter set and every probe that ran kept > 50% (the gate drops those, so the next decode carries a different set) | `sirius_gpu_scan_operator_data.cpp` (**pre-existing consumer code, outside the stack**) |
| `SIRIUS_PROTO_OP2_ENDPOINT=1` | endpoint negligible rule does not count a stronger filter the batch's decode already applied (test of "make it useful") | `dynamic_filter_merge.cpp` (pre) |
| `SIRIUS_PROTO_OP2_SKIP=join:cond` | oracle: do not build that key | publisher (measurement only) |
| `SIRIUS_PROTO_OP3_BLOOM_KEYS=join:keys` | oracle: size that join's Bloom keys on `keys` (via `fixed_bloom_keys`) | publisher (measurement only) |

### 2.1 Rule coverage: all 46 constructions

Checked with debug passes under `SHADOW=1 LATCH=1` (labels `diag_shadow`, `suite_dbg`: q1, q2, q3, q5, q7, q8, q10, q20, q21) plus
`diag0` gate logs (q9, q11, q16): every `[df_proto] shadow` line and every `[dynamic_filter_key]` line read.
q4, q12, q17, q19, q22 (and q18's two channels, q16, q11) have one filter per channel in the log-pass tables, where the rule cannot fire
(survival = 1); they were not re-run in debug with the switch.

| construction | rule | why |
|---|---|---|
| q8 j41 (Bloom 174 MiB) | **skip** | removed share 0.0063 |
| q21 j18 (Bloom 1394 MiB) | **skip** | removed share 0.0205 |
| q2 j28 (IN-list 2.0M) | build (missed) | publishes **before** the stronger j25 filter exists on the channel; order-dependent |
| q20 j49 (IN-list, 2 bindings) | build | not shadowed on its second channel |
| q9 j29 c0 / c1 | not judged | keys not unique -> estimate unreliable |
| the other 39 with a filter (+ q2 j42, q20 j45: targets drained) | build | removed share 0.80-0.99, or single filter on the channel |

No filter that any consumer applies today is skipped: every filter the rule removes was `skip negligible_removed_share` on
every batch and never run at decode in the tip-behaviour logs. Key-outcome lines otherwise identical to tip (44 filters instead of 46).

## 3. Timing

Protocol: fresh process per query, 6 iterations (1 warm + 5 hot, first hot dropped -> 4 samples per run), tip binary = `$R/repo`,
prototype = `$R/wt-op2` with the env switches, palindromic order per query (tip, variants..., variants reversed, tip), lock per run.
The machine was heavily shared (15-26 lock waiters), so runs of one block are 10-20 min apart.

### 3.1 q8, q21: shadow rule (`SHADOW=1`), and "make it useful" (`ENDPOINT=1`)

| q | tip med ms (n) [min-max] | shadow skip med (n) [min-max] | delta | per-pass | endpoint-apply med (n) | delta | results |
|---|---|---|---|---|---|---|---|
| 8 | 936.0 (8) [930-940] | 901.0 (8) [896-908] | **-35.0 ms (-3.74%)** | -34.4 / -37.1 | 936.4 (8) [930-938] | +0.4 (+0.04%) | identical |
| 21 | 1445.8 (4) [1435-1468] | 1142.4 (4) [1125-1155] | **-303.5 ms (-21.0%)** | -303.5 (one pass) | 1424.4 (8) [1418-1445] | -21.4 (-1.5%) | identical |

- q8: full ABBA (labels `t1_{tip,shadow,endpoint}_{A,B}`). Resolved: ranges do not overlap.
- q21: **one AB pass only for tip vs shadow** (`f1_tip_A`, `f1_proto_A`, proto = `SHADOW=1 LATCH=1`; the latch never changes
  on q21: 5 `latch=true` lines as at tip); the B pass of tip and proto did not get the lock (65-69 waiters) before this
  write-up. 4 hot samples each, ranges 280 ms apart, and the tip figure matches the earlier baseline (1436 ms), so the sign and size
  are not in doubt, but it is not ABBA. Endpoint variant has both passes, compared against tip A only.
- The gain equals the Publishing Task time of the removed construction: q8 j41 35.5-36 ms (PROFILE 3.4); q21 j18 267 ms finish +
  44 ms net insert = 311 ms.
- `ENDPOINT=1` on q21 (Bloom applied post-decode on the IN-list survivors): the filter is worth ~21 ms when applied; it costs
  ~311 ms to build here. On q8 it is worth nothing.

Bounce split (model figures from OPPORTUNITIES, not measured here):

| q | this machine (measured) | bounce excluded / P2P-equivalent (model) | after OP1 (finish hidden in slack), P2P-eq |
|---|---|---|---|
| 8 | -35.0 ms (3.7%) | ~ -15 ms (3.4 insert + ~12 finish at 68 ms/GB) (1.6%) | ~ -3.4 ms |
| 21 | -303.5 ms (21.0%) | ~ -139 ms (44 insert + ~95 finish) (9.7%) | ~ -63 ms (44 + 95 - 76 slack) |

### 3.2 q9: latch fix, oracle skip of the suppkey key, OP3 oracle

Full palindrome, 2 passes, 8 hot samples per variant (labels `t1_*`).

| variant | med ms (n) [min-max] | delta vs tip | per-pass deltas |
|---|---|---|---|
| tip | 1433.6 (8) [1402-1449] | | |
| `LATCH=1` | 1413.3 (8) [1401-1448] | -20.2 (-1.4%) | -32.1 / -0.6 |
| oracle `SKIP=29:1` (suppkey Bloom not built) | 1392.4 (8) [1379-1415] | **-41.1 (-2.9%)** | -54.8 / -10.6 |
| oracle skip + OP3 (`BLOOM_KEYS=29:10870459`: partkey Bloom 83 -> 21.7 MiB) | 1393.2 (8) [1375-1412] | -40.4 (-2.8%) | -48.1 / -25.0 |
| OP3 on both keys + `LATCH=1` (2 x 21.7 MiB) | 1416.1 (8) [1384-1427] | -17.5 (-1.2%) | -19.1 / -11.6 |

- Results identical in all 10 runs. Mechanics verified in debug runs: `diag_latch` (latch=false x16, then the partkey Bloom is
  decode-attached on 46 of 62 chunks), `diag_skip` (partkey decode-attached on 62 of 62, measured keep 0.054),
  `diag_op3` (`[dynamic_filter_key]` size=21740928 for both j29 keys).
- q9 was noisy in this block (tip A 1441, tip B about 1410; the machine had 15+ lock waiters). **Oracle skip is resolved in sign
  (-41 ms, both passes negative); the latch fix is not resolved (-20 ms, passes -32 / -0.6).** A second block (`t2`) was queued and
  did not get the lock.
- OP3 adds nothing measurable: skip vs skip+OP3 = +0.7 ms; latch vs latch+OP3 = +2.8 ms. Both inside noise. Drop.
- Bounce split: the latch gain is consumer-side decode work (no bounce): same on both columns. Of the oracle skip's -41 ms, the
  part beyond the latch is the 83 MiB Bloom's share of the finish (~16 ms here, ~6 ms P2P-equivalent) plus 1.57 B wasted probes.

### 3.3 Other queries under the final config (`SHADOW=1 LATCH=1`)

Single AB pass each (tip A, proto A; 4 hot samples per side). ABBA was started and then cut to AB because of lock contention.

| q | tip med ms [min-max] | proto med ms [min-max] | delta | results |
|---|---|---|---|---|
| 2 | 149.1 [147-150] | 148.6 [147-150] | -0.4 (-0.3%) | identical |
| 3 | 661.4 [658-739] | 657.5 [657-664] | -3.9 (-0.6%) | identical |
| 5 | 691.9 [681-710] | 692.0 [679-713] | +0.1 (0.0%) | identical |
| 16 | 315.1 [303-316] | 312.0 [307-317] | -3.1 (-1.0%) | identical |

All inside noise, as expected: the rule and the latch change nothing on these (debug passes: no `decision=skip`, `latch=true`
unchanged).

**Not run (lock never obtained): timing for q20, q11, q10, q7, q4, q17, q18 and the rest of the suite (q1, q6, q12-q15, q19, q22).**
For q7, q10, q20 (and q1, q2, q3, q5) the debug passes show the prototype takes no different decision than tip, and results are
identical to the baseline tip results; q4, q11, q12, q16-q19, q22 have single-filter channels where neither switch can act. So no
effect is expected, but it is not measured.

### 3.4 Suite estimate (sum of the measured per-query gains; the full suite was not re-timed)

| | this machine | bounce excluded / P2P-equivalent (model) |
|---|---|---|
| shadow rule (q8 + q21) | -338 ms = **2.2%** of 15372 | ~ -154 ms = 1.0% |
| + latch fix (q9, unresolved) | -20 ms = 0.13% | -20 ms |
| total | ~ -358 ms (2.3%) | ~ -174 ms (1.1%) |
| oracle bound for q9 instead of latch | -41 ms | ~ -31 ms |

The shadow-rule gain overlaps OP1: after OP1 the finish share that fits in slack is already hidden (q8 almost all; q21 76 of 267 ms).
Unlike OP1 it also removes the insert (q21 44 ms), the 15.3 GB of copies and 1.4 GiB x 4 GPUs of unspillable Bloom memory.

## 4. Correctness evidence

- `result.txt` byte-identical (md5) between tip and prototype in every timed run: q8 (6 runs), q9 (10), q21 (4), q2, q3, q5, q16;
  and between the debug passes with `SHADOW=1 LATCH=1` and the baseline tip results (`runs/baseline/baseline_tip`) for
  q1, q2, q3, q5, q7, q10, q20, q21. All runs rc=0.
- DF unit tests on the prototype build, switches off (`runs/op2/unit/`): `[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]`
  638 cases, `[publisher],[construction_plan],[physical_partition],[build_filter]` 125 cases, `[dynamic_filter][multi_gpu]` 31 cases:
  all passed. No test exercises the switches on.
- Lost filters: 44 of 46 remain; the 2 removed were never applied by any consumer at tip (section 1). Filters are optional for
  correctness by design.

## 5. Risks / what a real version needs

- **Order dependence.** The rule only sees filters already published on the channel. q2 j28 is missed because it publishes first.
  Decisions can differ run to run when two builds finish close together (performance only).
- **It inherits the consumer rule's trust in the static estimate.** If the stronger filter's estimate is wrong, the consumer drops
  the stronger one after measuring and would then apply the weaker one; with the shadow rule it no longer exists. Mitigated by
  using only estimates of unique keys on both sides; correlation between filters is still not modelled. q21 shows the price of a
  wrong skip is small (-21 ms benefit forgone) against the price of a wrong build (311 ms).
- **Must stay in step with the consumer.** The constant and formula are duplicated from `dynamic_filter_merge.cpp`; a real version
  shares one definition. If the consumer rule or the decode cap changes, the construction rule must follow.
- **Distributed / cross-rank merge.** The decision reads local channel state, so ranks could disagree; not handled (single process here).
- **Key outcome.** Prototype reports `reason=not_constructed`; needs its own reason token and a stats counter.
- **Latch fix** is in pre-existing scan code outside the stack and is a heuristic: a scan with a moderately selective static filter
  plus weak probes would retry compaction once more per filter generation. Controls for that were not timed.
- q9's real fix needs a distinct count or FK metadata for non-unique keys (`ps_suppkey`): not available at plan time today. A
  runtime variant (bounds + Bloom popcount at finish, drop the key before replication when distinct / (max - min + 1) >= 0.9) was
  considered and not built: it only saves the replicate half and adds accumulator work.

## 6. Recommendation

- **Shadow rule: land (after hardening).** 2.2% of the suite here, ~1.0% bounce-excluded, on two constructions; no result change;
  small code (about 45 lines in the session + a flag on the filter). Needs: the decision moved into `plan_construction` with a
  reason token (#2569), the session supplying the per-binding channel facts (#2570), one shared definition of the rule with the
  consumer, unit tests, and a second q21 pass + untimed controls.
- **Endpoint-apply: drop** (q8 +0.4 ms; q21 -21 ms but only matters if the Bloom is built).
- **Latch fix: needs more** (q9 -20 ms unresolved; outside the stack; hand to the scan owner with the q9 evidence).
- **OP3: drop** (no measurable gain even with an oracle).
- q2 j28 / q16 double probe: consumer-side, small (3 ms / ~5 ms); note only.
