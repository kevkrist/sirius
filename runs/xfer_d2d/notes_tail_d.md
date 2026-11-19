
## 9. Memory held during a finish (G=4, two pair islands, 8 MiB chunks = any Bloom >= 64 MiB)

| | pinned HOST | device scratch, root GPU | device scratch, other leader | other GPUs |
|---|---|---|---|---|
| tip, HOST staged (this machine) | 8 MiB (1 window) | 8 MiB (1 slot; 0 when S=0) | 0 | 0 |
| tip, PEER (full P2P) | 0 | 48 MiB (2 parities x 3 sources) | 0 | 0 |
| `star` | 8 MiB | 24 MiB (2 merge + 1 cross) | 16 MiB (2 merge) | 0 |
| `exchange` | 16 MiB (1 window per leader) | 24 MiB | 24 MiB | 0 |
| `pipe` | 32 MiB (2 windows per leader) | 32 MiB (2 merge + 2 cross) | 32 MiB | 0 |

- Slots scale with the chunk (clamp(B/8, 2, 8 MiB)), not with B. The root's scratch stays inside the bound the task
  already reserves (`publication_scratch_bytes` = 2 x (G-1) slots = 48 MiB at G=4).
- New: the other leader's scratch is leased at finish time through `scoped_replica_reservation` on a GPU the
  Publishing Task does not run on (a refusal falls back to the tip transports). It is held only for the finish.
- All of it is released before `finish` returns. The array memory itself (B per GPU) is unchanged.
- CUDA Best Practices 10.1.1 ("pinned memory should not be overused"): 32 MiB transient from the engine's existing
  fixed-block pinned pool, no new pinning.

## 10. Production issues (what a real [xfer-PR] must fix; the prototype is not mergeable)

1. **Scratch on the other leader is not in the plan's upfront lease.** Either add it to the session's reservation at
   `begin` (it is known then: islands depend only on the replica spaces) or accept the finish-time lease + fallback.
   The prototype also leases the cross slots when the other island contributes nothing (15-16 MiB unused).
2. **Per-finish object churn**: 6 streams, 22 events, 4 windows, 1 lease per finish = the ~0.06 ms fixed cost (6.1).
   Use `runtime_stream_pool` leases and keep the tip loop for tiny single-contributor filters.
3. **Topology is recomputed per finish** from `try_grant_pool_access` (N x (N-1) calls; `cudaMemPoolGetAccess`
   shows 20 calls per 2 finishes in nsys). Compute the islands once per context and hand them to the builder.
4. **Islands = greedy cliques of mutual access.** Exact for pair / full / none topologies; for a non-transitive access
   graph a greedy clique cover is legal but not necessarily the best.
5. **More than two islands** only has `cross_star` (serial HOST loop over the leaders). The right cross stage for N
   leaders is the other agent's `host_or` over N inputs (section 3).
6. **Failure paths**: `island_reduce`'s destructor drains its own streams before its scratch and windows go away, and
   `finish_impl`'s `join_on_failure` still joins the partial streams; the 4 accumulation fault-injection tests pass
   with the switch on, but they were written for the tip's call sequence: island-specific faults (failed peer copy on
   the other leader, failed event wait) have no test. `leak_storage` is not set for a failed island-stream join.
7. **Observability contract**: `publication.transport` still reports `HOST_STAGED` for an island publication and
   `workspace_observation` reports only the root's scratch and the summed windows. A third route value
   (`ISLAND`) and per-GPU scratch are needed before tests can assert on it.
8. **The cross stage is the prototype's weakest code**: three hand-written variants. In the real PR it should be one
   leg-graph primitive shared with the host pipeline (`proto/xfer-host`), with P2P legs and HOST legs as two leg kinds.
9. **Replica relay** relies on "HOST-staged copies are complete on return". If the host route becomes asynchronous
   (the other agent's `host_bounce_scope`), a relayed copy must wait on the relay's upload event: the relay then has
   to carry an event, not only a pointer.
10. **Concurrency with other traffic** (build CONCAT move, eager probe scans of S6): `pipe` uses both pair uplinks
    and all four ports at once for 50-80 ms on q21. MB_TRANSFER section 7 measured the contention of such schedules
    with a synthetic background; nothing was measured here with eager scans on.
11. Exact-key gather still pays `D2H, sync, H2D, sync` per slice from the two cross-pair GPUs (5.3 ms at 512 spread
    slices): owned by the host-bounce prototype (batched gather), not changed here.

## 11. Expected behaviour on other machine classes

| Machine | Bloom finish | Replicas / gather | Evidence |
|---|---|---|---|
| Full P2P (every pair has pool access) | one island -> `plan()` declines -> tip PEER pipeline, byte for byte. **No change, no gain, no cost** (one extra N^2 grant query) | relay never engages (every copy is already a peer copy) | G=2 pair: off = on within 0.2% (6.3); G=4 full P2P: by code only, no such machine here |
| Partial P2P (this box: 2 pairs) | islands; 185 -> 52-56 ms/GiB at S=3, 61 -> 40 at S=0 | one bounce instead of two per fan-out | sections 4-7 |
| No P2P | all-singleton islands -> `plan()` declines -> tip HOST loop. **No change**: the whole gain there has to come from the host pipeline (`proto/xfer-host`: 53-76 ms/GiB modelled in MB_TRANSFER) | relay never engages (no relay grants anyone) | by code only; a 3-GPU no-P2P set cannot be formed on this box |
| Full P2P, G >= 4, as a follow-up | the island code with a P2P cross leg would give 2 x B per port instead of the star's 3 x B (MB_TRANSFER: ~44 vs 62-67 ms/GiB, model) | - | not built: needs "islands" to mean a reduce tree, not access cliques |

CUDA Best Practices Guide, memory chapter, as applied: 10.1 "minimize data transfer between the host and the device"
(10 x B -> 4 x B through the host, the OR stays on the GPUs); 10.1.1 pinned memory, not overused (existing pool, 32 MiB);
10.1.2 asynchronous transfers overlapped with computation on non-default streams, staged per chunk (`pipe`: copies and
OR kernels of consecutive chunks overlap, one host wait at the end instead of 10 per chunk).
