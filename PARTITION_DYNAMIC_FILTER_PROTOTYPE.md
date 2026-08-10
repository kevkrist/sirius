# Partition-specific dynamic-filter prototype

## Conclusion

For a partitioned hash join, the partition-specific Bloom belongs at the probe `CONCAT`, after the
probe exchange. At that point the operator knows the exact hash partition and device, so it can use
the corresponding filter without replicating or combining filters. This removes non-members before
the hash join while leaving partition sizing, scattering, and batching unchanged.

The build-side `CONCAT` is the matching publication boundary. Every post-exchange build fragment
for partition `p` contributes to filter `p`; finalizing that `CONCAT` proves that all fragments have
arrived and seals the bank. Applying earlier would risk false negatives from an incomplete Bloom.

## Execution contract

1. The planner admits supported equality keys only when dynamic filtering is enabled, the build
   subtree carries evidence, and the join is `INNER`, `RIGHT`, or `SEMI`. Partition-specific mode
   retains those admitted keys but creates no scan or direct-filter targets.
2. The hash join creates a pending filter bank. Runtime eligibility requires a non-broadcast `HASH`
   strategy with more than one partition; all other strategies disable the bank and pass through.
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

On one GPU, all partition-local Blooms live in that device's allocator and each probe partition
uses only its identically numbered Bloom.

On multiple GPUs, the design remains local after the exchange: a Bloom is built in the actual
device memory that receives partition `p`, and the matching probe partition must arrive on that
same device. No cross-GPU Bloom merge or replication is required. An unexpected placement mismatch
passes the probe batch through and increments telemetry rather than risking incorrect filtering.

The focused tests cover bank lifecycle and scheduler behavior, plus a forced multi-partition
single-GPU query with master-off/master-on and CPU-result parity. Physical multi-GPU correctness,
placement stress, and performance evaluation are intentionally deferred to a multi-GPU machine.

## A/B measurement

Use identical partition settings and compare these modes:

```sql
-- Baseline
SET enable_dynamic_filter = false;

-- Existing global post-scan filter
SET enable_dynamic_filter = true;
SET enable_dynamic_filter_partition_specific = false;
SET enable_dynamic_filter_multi_partition = true;

-- Partition-specific post-exchange filter
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

## Prototype limits

- Bloom membership filters only; no zone-map or adaptive profitability gate.
- Post-exchange filtering only, and only for eligible multi-partition hash joins.
- Planner admission and the existing domain-coverage policy are reused; unsafe join types are
  rejected defensively again by the physical operator.
- Optional failures never fail the query, so telemetry must be checked when interpreting a run.
- Multi-GPU execution and performance have not been validated on this machine.
