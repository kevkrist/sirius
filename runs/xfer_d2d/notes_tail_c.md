
## 7. (c) nsys on q21 and q3: finish wall, bytes by transport, sync count

`bin/nsys_q.sh <q>` (3 iterations per arm, iteration 0 warm; captures under `runs/xfer_d2d/ns_tip`, `ns_pipe`),
analysis `bin/nsys_xport.py` (copies issued by the finish thread inside `dynfilter::accum::finish`, followed by
correlation id; a `cudaMemcpyBatchAsync` = one HOST-staged leg, direction from the memory kinds; CUPTI's `copyKind`
is not usable for batch copies) and `bin/nsys_tab.py`. Two hot iterations each; the two iterations agree to < 1 ms and
to the byte.

| q | arm | `accum::finish` wall ms per iteration (each finish) | host waits (`cudaStreamSynchronize`) | D2H MiB | H2D MiB | P2P MiB | query wall under nsys ms |
|---|---|---|---|---|---|---|---|
| 21 | tip | 307.7 (281 + 27) | 1932 | 6107 | 9161 | 0 | 1450 |
| 21 | proto `pipe` | 84.4 (76 + 9) | 38 | 3054 | 3054 | 6107 | 1258 |
| 3 | tip | 62.2 (58 + 4.2) | 404 | 1179 | 1855 | 0 | 667 |
| 3 | proto `pipe` | 23.4 (20.4 + 3.0) | 34 | 618 | 618 | 1236 | 634 |

- q21: finish 308 -> 84 ms (3.6x), host-staged bytes 15.3 GiB -> 6.1 GiB (10 x B -> 4 x B), P2P 0 -> 6.1 GiB (4 x B),
  host waits 1932 -> 38 (19 per finish: island streams + partial streams at the end, allocation joins).
  q3: 62 -> 23 ms, host-staged 3.0 GiB -> 1.2 GiB, waits 404 -> 34.
- Engine vs microbench: q21's 1394 MiB array takes 76 ms in the engine under nsys (55 ms/GiB; MB4 52), q3's 281 MiB
  array 20.4 ms (MB4 14.3 ms; the tip's 58 ms is also 7 ms above MB4's 51: the finish first waits for the insert
  kernels still running on the partial streams).
- Query wall under nsys: q21 -192 ms, q3 -33 ms: consistent with the ABBA deltas (-198, -37 ms). About 30 ms of q21's
  223 ms finish saving does not reach the wall (the second, 133 MiB filter is not fully on the critical path).
- Debug-log pass (`bin/logpass.sh 3 21`, 2 iterations): all 8 Bloom finishes logged `through island peer copies +
  HOST pipelined exchange`, roots GPU 0, 1, 2 and 3 all occurred; 0 fallback lines. Scratch on the other leader:
  32 MiB (2 x 2 slots of 8 MiB) for S=3, 15 MiB for the 57 MiB S=0 filters (allocated but unused: see section 10).

## 8. Correctness gates

| Gate | What | Result |
|---|---|---|
| `[mbid]` G=4 (`mb/gate_g4.log`) | 4 sizes (1, 3, 20, 57 MiB; 2 keys of different size -> 1 to 8+ chunks, both chunk parities) x all 16 contribution masks x roots {0, 3} x modes {off, star, exchange, pipe}: every replica on every GPU compared byte for byte with the OR of the partials read before `finish`, and with the `off` replica | 512 cases, **0 bad** |
| `[mbid]` G=3 on GPUs 0,1,2 and on GPUs 0,2,3 | islands {pair, single} with the root in the pair / as the single | 128 + 128 cases, 0 bad |
| `[xfer_relay]` (`test_sirius_dynamic_filter_mgpu.cpp`) | hash IN-list (200k keys), small IN-list (7 keys), one-shot Bloom (200k keys), roots {0, 3}: mask of a 700k-row probe on every GPU, switch on vs off, identical (exact kinds also checked against the exact key set; Bloom includes its false positives) | 24 masks identical, 6 copies relayed (1 per filter, as designed) |
| TPC-H results | `result.txt` md5 of every ABBA process, tip vs prototype | identical for all 22 queries (section 5) |
| Existing unit tests, switch `pipe` | `[dynamic_filter][multi_gpu]~[.]` (32 cases incl. the 4 accumulation fault-injection cases) | UNIT_PIPE_RESULT |
| Existing unit tests, switch off (same binary) | same filter | UNIT_OFF_RESULT |
| Other multi-GPU tests, switch `pipe` | `[multi_gpu]~[dynamic_filter]~[.]` | UNIT_MGALL_RESULT |
| `[cuda_status_fault]`, switch `pipe` / off | fault injection (replica + accumulation) | UNIT_FAULT_RESULT |

Not run: the single-GPU DF tests (`plan()` declines below 3 GPUs and the relay never engages without a HOST-staged
copy, so they cannot reach new code), the integration shards, `make test`.
