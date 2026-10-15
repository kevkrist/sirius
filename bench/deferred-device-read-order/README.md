# Deferred device read ordering benchmark

When cuDF queues reads for files A, B, and C and first waits for A, Sirius issues A, B, then C before waiting for A's completion. This own-first policy prioritizes the requested batch while preserving sibling issuance on the first wait.

The benchmark compares explicit `siblings_first` (B, C, A) and `own_first` (A, B, C) variants, independently of the current production order. Sibling-first can overlap B/C work with a prefetch wait inside A's issue call. It can also delay A behind a blocked sibling or a limited backend's queued work. Both variants issue all batches before checking A's terminal state and waiting for A's completion. A's data can therefore be ready before A's `get()` returns when issuing a sibling blocks.

The retained measurements predate the production switch to own-first. Their source snapshots and hashes identify the exact historical variants; they are not measurements of the subsequent failure-path adjustment that preserves sibling issuance after an own-batch failure.

The completed [measurement report](RESULTS.md) includes observed benefits, costs, raw samples, and request traces.

## Reproduce

Run from the repository root after configuring the normal release build:

```bash
pixi run cmake --build build/release --target deferred_order_siblings_first deferred_order_own_first --parallel 8
pixi run python bench/deferred-device-read-order/run.py --build build/release --output /tmp/deferred-order-results
```

The output directory must not already exist. A focused case is:

```bash
pixi run build/release/test/io/deferred_order_siblings_first /tmp/order-siblings blocked_a 2 4194304 20 3 20000 2000
pixi run build/release/test/io/deferred_order_own_first /tmp/order-own blocked_a 2 4194304 20 3 20000 2000
```

Arguments are output directory, scenario (`none`, `blocked_a`, `blocked_b`), worker count, bytes per source, measured iterations, warmup iterations, prefetch gate in microseconds, and service delay in microseconds. A per-iteration 20-second process alarm and a runner subprocess timeout fail a stalled run rather than leaving a blocked future alive indefinitely.

## Work and measurement

The executable uses the actual `sirius_datasource` futures, `prefetching_cache`, `deferred_device_batch::await`, and registry. Three deterministic local fixture files each supply four slices on the same explicit CUDA stream. One or two persistent backend workers perform real `pread` into pinned staging, copy host cache fragments when requested, perform actual H2D copies, and settle the production completion coordinator. Every byte of all three device destinations is downloaded and verified after every iteration, including warmups.

The queue accepts at most eight requests and fails on overflow; this workload submits three requests. The workers allocate pinned staging and start before the timed region. Their service is bounded by worker count; no thread is created per request. Each iteration gets a fresh Sirius cache. Only the chosen blocked source receives prefetch hints, preparation, and a real prefetch request before reads are queued. Its service is gated until 20 ms after the first wait starts. That gate occupies a service worker. The other worker can process demand reads while it waits. The files are freshly written before each process's samples, so the operating system page cache is warm; these are not cold-disk measurements.

The runner covers all three scenarios, one and two workers, 64 KiB and 4 MiB per source, and service delays of 0 and 2 ms per request. Both the prefetch gate and optional service delay are **injected latency**. The 2 ms case shows how ordering interacts with slower service; it is not a measured disk or network latency. Each configuration has two rounds with policy order reversed in the second round, two warmup iterations per process, and ten measured iterations per round (20 samples per policy/configuration).

A benchmark-only linker wrapper calls the real `cudf::detail::memcpy_batch_async`, then appends a CUDA host function that only adds the copied byte count and records a steady-clock timestamp when a source's last copy finishes. This same observer is used for misses and cache hits in both policies. The callback does not call CUDA, allocate, or throw. This measures observed stream completion, with host-callback scheduling overhead; it is not a GPU hardware timestamp. Observer overhead is part of both measured variants.

The result files contain:

- `samples.csv`: raw A data-ready time, first A `get()` return, all futures consumed, and B/C data-ready times relative to the first wait.
- Per-process `trace.csv`: backend enqueue, service start, and service complete timestamps by source, with a flag separating prefetch work from demand work. Prefetch enqueue precedes the timing origin and can be negative. A cache hit has no demand backend enqueue; its separate ready metric records the actual H2D completion.
- `summary.csv`: median and nearest-rank p95 for every metric and configuration, with sample counts.
- `commands.json`, `metadata.json`, and logs: exact subprocess arguments, environment/GPU identity, hashes of executables and relevant sources, cache regime, and failures.
- `generated/`: the current production batch snapshot, explicit sibling-first and own-first sources, exact diffs, source hashes, and link wrapper symbol declaration.

`prepare.py` checks the production `await` anchors and moves only the own `issue_if_open()` invocation relative to the sibling issue block. Terminal-state checking stays after both issuance steps. It fails when the expected structure changes. Each named policy object is linked before the current `sirius_core` archive; the variant matching production is byte-identical to its source snapshot. No production source or policy setting is changed. The runner rejects a stale source manifest. Rebuild both targets after editing the implementation.

Inspect issue order and overlap in the traces, not just elapsed time: with two workers and blocked A, sibling-first can finish B/C while A's prefetch is gated. With blocked B, own-first can complete A's copies early, but its A `get()` still waits for the blocked sibling's issue call to return. One worker removes much of the opportunity to overlap work with a blocked prefetch because that request occupies the only service slot.

These results concern a controlled scheduling workload. They do not establish how often production scans block on A versus a sibling, or demonstrate a TPC-H/query-level speedup.

## Developer assigned-reading receipt

Read `/home/ubuntu/.codex/agents/developer.toml`, repository `AGENTS.md`, and the architect's `docs/design/deferred-device-read-repair.md` before implementation. The parent searched for `/module-context` and reported it unavailable; local dependency APIs were inspected instead.

Selective book reading, rather than a claim of reading entire books:

- `~/AgentDocs/C++ Software Design.md`: contents; Guideline 4, lines 1888-2254 (testability and separation of concerns); Guideline 9 introduction, lines 3566-3625 (ownership of abstractions and dependency inversion). The benchmark uses the existing backend interface and isolates measurement from the production policy.
- `~/AgentDocs/C++20 -- The Complete Guide.md`: contents; chapter 18, lines 21819-22006 and 22600-22684 (`std::jthread` ownership and automatic joining). Workers are a fixed pool of owned joining threads.
- `~/AgentDocs/CppCoreGuidelines.md`: section index; C.36/C.37, lines 5426-5510 (nonthrowing destruction); error-handling introduction/E.1-E.4, lines 15900-16070; CP.20, lines 14696-14727 (RAII locking); CP.22, lines 14787-14830 (callbacks and locks); CP.42, lines 15169-15223 (wait conditions). Worker failure settles outstanding request credits, and the process deadline bounds hangs beyond ordinary RAII recovery.

Read Super Sirius README, architecture overview, execution flow through scan/executor lifecycle, memory tier/stream lifetime sections, and scan I/O architecture, deferred read path, and cache sections. Inspected `controlled_backend.hpp`, `io_request.hpp`, `types.hpp`, `templated_ioctx.hpp`, existing deferred-read fixtures, `device_copy_batch`, cache arbitration, local RMM `cuda_device.hpp`, cuDF `cuda_memcpy.hpp`, CUDA runtime declarations, and cuCascade reservation-manager configuration. Read the official [cuDF C++ documentation guide](https://raw.githubusercontent.com/rapidsai/cudf/main/cpp/doxygen/developer_guide/DOCUMENTATION.md) and used project copyright headers, ASCII prose, and comments at the scope owning each contract.
