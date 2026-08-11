# Partition-specific dynamic-filter prototype

## Conclusion

The global multi-partition Bloom should remain the primary implementation. On the single-GPU
SF1000 TPC-H queries where a multi-build-batch Bloom was actually published under the default
256 MiB cap, the global design was neutral on Q8 and 19.8% faster than the one-shot-only control on
Q10. The partition-specific prototype produced no robust improvement on either query and was 18.6%
slower than global on Q10.

If a partition-specific Bloom is retained as a fallback or experiment, the probe `CONCAT` is the
correct local application point. It is the first post-exchange operator that knows the exact hash
partition and owning device, so it can select the matching filter without cross-GPU merge or
replication. That correctness result does not make post-exchange filtering the best performance
design: scan, partition, exchange, repository, and CONCAT work have already been paid before the
filter runs.

The plan also retains the ordinary scan and direct-filter targets. If runtime sizing selects one
partition or broadcast, the local bank is disabled and the existing whole-build one-shot publisher
may use those targets. The probe `CONCAT` then follows its ordinary scheduling, accounting, and
merge-only memory estimate.

The build-side `CONCAT` is the matching publication boundary. Every post-exchange build fragment
for partition `p` contributes to filter `p`; finalizing that `CONCAT` proves that all fragments have
arrived and seals the bank. Applying earlier would risk false negatives from an incomplete Bloom.

## Execution contract

1. The planner admits supported equality keys only when dynamic filtering is enabled, the build
   subtree carries evidence, and the join is `INNER`, `RIGHT`, or `SEMI`. Partition-specific mode
   retains both those admitted keys and their ordinary scan or direct-filter targets.
2. The hash join creates a pending filter bank. Runtime eligibility requires a non-broadcast `HASH`
   strategy with more than one partition. All other strategies disable the bank, allowing the
   whole-build one-shot path where applicable and making probe `CONCAT` a true local-filter bypass.
3. The blocking build `PARTITION` uses its exact whole-build snapshot to arm the bank with global
   build rows, partition count, and GPU count. This determines Bloom geometry without retaining or
   coalescing input batches.
4. At the hash join build port, every batch emitted by the build `CONCAT` contributes its admitted
   keys to the Bloom for the batch's exact partition. Contribution uses the batch's actual GPU
   memory space and a durable stream after its writer event has completed.
5. Build `CONCAT` finalization seals immutable filter sets. If the bank was never armed, finalization
   disables it so the probe side cannot wait indefinitely.
6. While an eligible bank is incomplete, the probe `CONCAT` reports the build `CONCAT` as its input
   dependency. This is scheduler ordering only; it does not force `concat_all` or change batching.
7. Probe `CONCAT` selects `(partition, actual_device)` from the sealed bank and applies membership
   filters to that batch. The partition tag and telemetry are preserved for downstream hash join.

Any missing fragment, residency/view/event failure, device mismatch, unsupported key, allocation
failure, or selection failure is fail-open. A fragment failure abandons its entire partition before
seal so an incomplete Bloom can never reject a valid row. Other partitions may remain usable.

## Configuration

The prototype is default-off:

```sql
SET enable_dynamic_filter = true;
SET enable_dynamic_filter_multi_partition = false;
SET enable_dynamic_filter_partition_specific = true;
```

`enable_dynamic_filter_partition_specific` and `enable_dynamic_filter_multi_partition` are mutually
exclusive. Disable the active strategy before enabling the other. The master
`enable_dynamic_filter` switch dominates both.

The existing `max_dynamic_filter_bloom_bytes_per_gpu` policy also caps this bank. At arm time the
estimate is:

```text
estimated_bytes(ceil(global_build_rows / partitions))
  * admitted_keys
  * ceil(partitions / GPUs)
```

Overflow or a value above the cap disables the optional bank. The calculation is deliberately
conservative and models the worst partition count assigned to one GPU.

## Single- and multi-GPU behavior

On one GPU, a multi-partition join keeps all partition-local Blooms in that device's allocator and
each probe partition uses only its identically numbered Bloom. A one-partition join disables the
local bank and retains the ordinary one-shot behavior.

On multiple GPUs, the design remains local after the exchange: a Bloom is built in the actual
device memory that receives partition `p`, and the matching probe partition must arrive on that
same device. No cross-GPU Bloom merge or replication is required. An unexpected placement mismatch
passes the probe batch through and increments telemetry rather than risking incorrect filtering.

The focused tests cover bank lifecycle and scheduler behavior, plus a forced multi-partition
single-GPU query with master-off/master-on and CPU-result parity. Physical multi-GPU correctness,
placement stress, and performance evaluation are intentionally deferred to a multi-GPU machine.

## A/B measurement

Use identical partition settings and compare four modes. The one-shot-only mode is the control for
isolating either multi-partition extension:

```sql
-- Master-off baseline
SET enable_dynamic_filter = false;

-- Existing one-shot filters only
SET enable_dynamic_filter = true;
SET enable_dynamic_filter_multi_partition = false;
SET enable_dynamic_filter_partition_specific = false;

-- One-shot plus global multi-partition post-scan filter
SET enable_dynamic_filter = true;
SET enable_dynamic_filter_partition_specific = false;
SET enable_dynamic_filter_multi_partition = true;

-- One-shot plus partition-specific post-exchange filter
SET enable_dynamic_filter_multi_partition = false;
SET enable_dynamic_filter_partition_specific = true;
```

Force a real partitioned, non-broadcast hash join for prototype measurements with suitably small
`hash_partition_bytes`, `max_broadcast_join_size`, and `max_build_hash_table_bytes`, but keep all
three values identical across runs. Do not force `concat_all`.

To exercise multi-fragment accumulation, also reduce `scan_task_batch_size` and
`concat_batch_bytes` identically in every mode.

The `partition_dynamic_filter_` statistics report admitted/gated keys, partitions and filters built,
successful build fragments and rows, probe batches, rows before and after filtering, readiness
waits, failures, device mismatches, budget skips, and partitions whose rows exceeded the geometry.
Compare wall-clock time together with `probe_rows_out / probe_rows_in`; a low row ratio without an
execution gain indicates that post-exchange filtering cost outweighs avoided hash probes for that
workload.

### SF1000 single-GPU result

The comparison used one NVIDIA GB300, host-pinned Parquet SF1000, 1 GiB scan/hash/CONCAT batches,
`max_build_hash_table_bytes = 900 MiB`, the default 256 MiB Bloom cap, grouped execution, and seven
iterations per mode. Iteration zero was discarded, leaving six warm samples. Timing used
error-only logging to avoid making the amount of pruned task-level warning traffic part of the
result. All four modes returned byte-identical results.

Q8 and Q10 were the complete qualifying set under that cap. Separate info-logged activation and
validation runs under the same host-pinned settings showed Q8 publish its global Bloom from three
exact build contributions (91,139,462 rows), while Q10 published from 3--10 contributions
(114,709,814 rows). Other candidate queries either used only the existing one-shot path or reached
multi-partition accumulation but skipped Bloom construction at the cap.

| Query | Master off | One-shot only | Global multi | Partition local |
|---|---:|---:|---:|---:|
| Q8 | 2.759 s | 1.196 s | 1.211 s | 1.199 s |
| Q10 | 2.101 s | 1.953 s | 1.567 s | 1.924 s |

The enabled-mode Q8 ranges overlap; the 0.2--1.2% median differences are smaller than observed
variation. On Q10, global was 19.8% faster than one-shot-only and 18.6% faster than local, with no
overlap between the global range (1.512--1.577 s) and the local range (1.888--1.987 s). Local's
1.5% Q10 improvement over one-shot-only was within overlapping run variance.

This result supports shipping the global design behind its existing flag and not replacing it with
the current CONCAT-local design. On multi-GPU, local filtering may still be useful when a replicated
global Bloom exceeds the per-GPU cap but distributed local banks fit, or when reduction and
replication dominate; that requires physical multi-GPU measurement. A future local experiment
should filter received fragments before CONCAT materialization, or fuse filtering into
receive/merge, rather than adding a second materialization after merge.

## Prototype limits

- Bloom membership filters only; no zone-map or adaptive profitability gate.
- The partition-local path filters post-exchange and only for eligible multi-partition hash joins;
  single-partition and broadcast strategies retain the existing whole-build scan/direct-target
  path.
- Planner admission and the existing domain-coverage policy are reused; unsafe join types are
  rejected defensively again by the physical operator.
- Optional failures never fail the query, so telemetry must be checked when interpreting a run.
- Multi-GPU execution and performance have not been validated on this machine.
