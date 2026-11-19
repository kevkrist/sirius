
| q | range | n | wall ms | max ms | CUDA API calls (per range) | sync calls / ms | kernels: name n GPU-ms | memset MB | memcpy MB by kind (GPU copy ms) | top APIs |
|---|---|---|---|---|---|---|---|---|---|---|
| 21 | accum::contribute | 9 | 0.33 | 0.06 | 72 (8.0) | 0 / 0.00 | add_if_n 9 129.25 | 0 | - | StreamWaitEvent 18, StreamGetDevice 9, EventCreateWithFlags 9, cuKernelGetName 9, LaunchKernel 9 |
| 21 | accum::finish | 2 | 293.33 | 266.87 | 5014 (2507.0) | 1932 / 274.94 | sirius::op:: 576 2.88 | 0 | DtoH 1527 (28.1); HtoD 1660 (57.5); PtoP 12081 (255.3) | StreamSynchronize 1932, MemcpyBatchAsync 1920, cuKernelGetName 576, LaunchKernel 576, MemPoolGetAccess 4 |
| 21 | construction::begin | 4 | 0.71 | 0.21 | 96 (24.0) | 0 / 0.00 | - | 6113 | - | MallocFromPoolAsync 24, MemsetAsync 24, StreamCreateWithFlags 16, EventCreateWithFlags 16, EventRecord 16 |
| 21 | replicate_devices | 4 | 0.92 | 0.78 | 48 (12.0) | 14 / 0.65 | static_kernel 3 0.01 | 0 | DtoH 0 (0.0); HtoD 3 (0.1); PtoP 27 (0.5) | StreamSynchronize 14, MallocFromPoolAsync 6, StreamGetDevice 6, MemPoolGetAccess 6, MemcpyAsync 4 |
| 21 | stage::contribute | 2 | 0.10 | 0.07 | 16 (8.0) | 0 / 0.00 | static_kernel 2 0.00 | 0 | - | StreamWaitEvent 4, StreamGetDevice 2, cuKernelGetName 2, LaunchKernel 2, EventCreateWithFlags 2 |
| 21 | stage::gather | 2 | 0.18 | 0.10 | 52 (26.0) | 4 / 0.03 | - | 0 | DtoH 0 (0.0); PtoP 0 (0.0) | StreamWaitEvent 10, EventRecord 10, MemcpyAsync 8, EventCreateWithFlags 8, EventDestroy 8 |
| 3 | accum::contribute | 5 | 0.18 | 0.08 | 40 (8.0) | 0 / 0.00 | add_if_n 5 21.59 | 0 | - | StreamWaitEvent 10, StreamGetDevice 5, EventCreateWithFlags 5, cuKernelGetName 5, LaunchKernel 5 |
| 3 | accum::finish | 2 | 62.37 | 58.27 | 1020 (510.0) | 404 / 58.45 | sirius::op:: 108 0.52 | 0 | DtoH 280 (5.2); HtoD 618 (12.3); PtoP 2135 (51.5) | StreamSynchronize 404, MemcpyBatchAsync 392, cuKernelGetName 108, LaunchKernel 108, MemPoolGetAccess 4 |
| 3 | construction::begin | 2 | 0.36 | 0.23 | 40 (20.0) | 0 / 0.00 | - | 1351 | - | StreamCreateWithFlags 8, MallocFromPoolAsync 8, MemsetAsync 8, EventCreateWithFlags 8, EventRecord 8 |
| 3 | replicate_devices | 2 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |
| 7 | accum::contribute | 5 | 0.22 | 0.09 | 40 (8.0) | 0 / 0.00 | add_if_n 5 18.36 | 0 | - | StreamWaitEvent 10, StreamGetDevice 5, EventCreateWithFlags 5, cuKernelGetName 5, LaunchKernel 5 |
| 7 | accum::finish | 2 | 54.16 | 52.60 | 994 (497.0) | 394 / 50.39 | sirius::op:: 105 0.55 | 0 | DtoH 290 (5.3); HtoD 568 (13.5); PtoP 2016 (45.8) | StreamSynchronize 394, MemcpyBatchAsync 382, cuKernelGetName 105, LaunchKernel 105, MemPoolGetAccess 4 |
| 7 | construction::begin | 5 | 0.87 | 0.19 | 124 (24.8) | 0 / 0.00 | - | 1217 | - | MallocFromPoolAsync 32, MemsetAsync 32, StreamCreateWithFlags 20, EventCreateWithFlags 20, EventRecord 20 |
| 7 | replicate_devices | 5 | 1.73 | 1.40 | 69 (13.8) | 21 / 1.37 | static_kernel 3 0.01 | 0 | DtoH 0 (0.0); HtoD 6 (0.1); PtoP 55 (1.0) | StreamSynchronize 21, MallocFromPoolAsync 9, StreamGetDevice 9, MemPoolGetAccess 9, MemcpyAsync 8 |
| 7 | stage::contribute | 3 | 0.13 | 0.09 | 24 (8.0) | 0 / 0.00 | static_kernel 3 0.00 | 0 | - | StreamWaitEvent 6, StreamGetDevice 3, cuKernelGetName 3, LaunchKernel 3, EventCreateWithFlags 3 |
| 7 | stage::gather | 3 | 0.25 | 0.09 | 78 (26.0) | 6 / 0.04 | - | 0 | DtoH 0 (0.0); PtoP 0 (0.0) | StreamWaitEvent 15, EventRecord 15, MemcpyAsync 12, EventCreateWithFlags 12, EventDestroy 12 |
| 9 | accum::contribute | 7 | 0.26 | 0.04 | 64 (9.1) | 0 / 0.00 | add_if_n 11 3.92 | 0 | - | StreamWaitEvent 14, cuKernelGetName 11, LaunchKernel 11, StreamGetDevice 7, EventCreateWithFlags 7 |
| 9 | accum::finish | 2 | 36.13 | 32.45 | 754 (377.0) | 296 / 33.42 | sirius::op:: 82 0.31 | 0 | DtoH 176 (3.2); HtoD 363 (7.1); PtoP 1285 (30.5) | StreamSynchronize 296, MemcpyBatchAsync 284, cuKernelGetName 82, LaunchKernel 82, MemPoolGetAccess 4 |
| 9 | construction::begin | 2 | 0.43 | 0.28 | 48 (24.0) | 0 / 0.00 | - | 746 | - | MallocFromPoolAsync 12, MemsetAsync 12, StreamCreateWithFlags 8, EventCreateWithFlags 8, EventRecord 8 |
| 9 | replicate_devices | 2 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |
| 10 | accum::contribute | 11 | 0.47 | 0.08 | 88 (8.0) | 0 / 0.00 | add_if_n 11 5.39 | 0 | - | StreamWaitEvent 22, StreamGetDevice 11, EventCreateWithFlags 11, cuKernelGetName 11, LaunchKernel 11 |
| 10 | accum::finish | 2 | 37.20 | 20.86 | 672 (336.0) | 262 / 34.45 | sirius::op:: 75 0.40 | 0 | DtoH 194 (3.6); HtoD 389 (7.4); PtoP 1360 (32.3) | StreamSynchronize 262, MemcpyBatchAsync 250, cuKernelGetName 75, LaunchKernel 75, MemPoolGetAccess 4 |
| 10 | construction::begin | 2 | 0.41 | 0.24 | 40 (20.0) | 0 / 0.00 | - | 777 | - | StreamCreateWithFlags 8, MallocFromPoolAsync 8, MemsetAsync 8, EventCreateWithFlags 8, EventRecord 8 |
| 10 | replicate_devices | 2 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |
| 8 | accum::contribute | 5 | 0.14 | 0.03 | 40 (8.0) | 0 / 0.00 | add_if_n 5 11.17 | 0 | - | StreamWaitEvent 10, StreamGetDevice 5, EventCreateWithFlags 5, cuKernelGetName 5, LaunchKernel 5 |
| 8 | accum::finish | 2 | 40.37 | 36.23 | 656 (328.0) | 264 / 37.74 | sirius::op:: 66 0.30 | 0 | DtoH 174 (3.2); HtoD 579 (12.0); PtoP 1215 (29.9) | StreamSynchronize 264, MemcpyBatchAsync 252, cuKernelGetName 66, LaunchKernel 66, MemPoolGetAccess 4 |
| 8 | construction::begin | 5 | 1.71 | 1.84 | 124 (24.8) | 0 / 0.00 | - | 945 | - | MallocFromPoolAsync 32, MemsetAsync 32, StreamCreateWithFlags 20, EventCreateWithFlags 20, EventRecord 20 |
| 8 | replicate_devices | 5 | 2.53 | 2.17 | 69 (13.8) | 21 / 2.17 | static_kernel 3 0.02 | 0 | DtoH 0 (0.0); HtoD 20 (0.4); PtoP 81 (1.5) | StreamSynchronize 21, MallocFromPoolAsync 9, StreamGetDevice 9, MemPoolGetAccess 9, MemcpyAsync 8 |
| 8 | stage::contribute | 4 | 0.14 | 0.04 | 32 (8.0) | 0 / 0.00 | static_kernel 4 0.01 | 0 | - | StreamWaitEvent 8, StreamGetDevice 4, cuKernelGetName 4, LaunchKernel 4, EventCreateWithFlags 4 |
| 8 | stage::gather | 3 | 0.32 | 0.15 | 82 (27.3) | 7 / 0.08 | - | 0 | DtoH 0 (0.0); PtoP 4 (0.1) | StreamWaitEvent 15, EventRecord 15, MemcpyAsync 12, EventCreateWithFlags 12, EventDestroy 12 |
| 5 | accum::contribute | 5 | 0.17 | 0.06 | 40 (8.0) | 0 / 0.00 | add_if_n 5 2.47 | 0 | - | StreamWaitEvent 10, StreamGetDevice 5, EventCreateWithFlags 5, cuKernelGetName 5, LaunchKernel 5 |
| 5 | accum::finish | 2 | 21.09 | 17.02 | 370 (185.0) | 154 / 19.58 | sirius::op:: 33 0.13 | 0 | DtoH 87 (1.6); HtoD 144 (4.8); PtoP 866 (19.3) | StreamSynchronize 154, MemcpyBatchAsync 142, cuKernelGetName 33, LaunchKernel 33, MemPoolGetAccess 4 |
| 5 | construction::begin | 4 | 0.71 | 0.23 | 96 (24.0) | 0 / 0.00 | - | 576 | - | MallocFromPoolAsync 24, MemsetAsync 24, StreamCreateWithFlags 16, EventCreateWithFlags 16, EventRecord 16 |
| 5 | replicate_devices | 4 | 0.35 | 0.18 | 42 (10.5) | 14 / 0.16 | - | 0 | DtoH 0 (0.0); HtoD 0 (0.0); PtoP 0 (0.0) | StreamSynchronize 14, MemcpyAsync 8, MallocFromPoolAsync 6, StreamGetDevice 6, MemPoolGetAccess 6 |
| 5 | stage::contribute | 2 | 0.09 | 0.07 | 16 (8.0) | 0 / 0.00 | static_kernel 2 0.00 | 0 | - | StreamWaitEvent 4, StreamGetDevice 2, cuKernelGetName 2, LaunchKernel 2, EventCreateWithFlags 2 |
| 5 | stage::gather | 2 | 0.18 | 0.10 | 52 (26.0) | 4 / 0.03 | - | 0 | DtoH 0 (0.0); PtoP 0 (0.0) | StreamWaitEvent 10, EventRecord 10, MemcpyAsync 8, EventCreateWithFlags 8, EventDestroy 8 |
| 2 | construction::begin | 5 | 1.11 | 0.39 | 156 (31.2) | 0 / 0.00 | - | 72 | - | MallocFromPoolAsync 48, MemsetAsync 48, StreamCreateWithFlags 20, EventCreateWithFlags 20, EventRecord 20 |
| 2 | replicate_devices | 4 | 4.72 | 3.10 | 96 (24.0) | 28 / 4.11 | static_kernel 6 0.04 | 0 | DtoH 0 (0.0); HtoD 27 (0.5); PtoP 186 (3.5) | StreamSynchronize 28, MallocFromPoolAsync 12, StreamGetDevice 12, MemPoolGetAccess 12, MemcpyBatchAsync 8 |
| 2 | stage::contribute | 10 | 0.42 | 0.06 | 88 (8.8) | 0 / 0.00 | static_kernel 14 0.03 | 0 | - | StreamWaitEvent 20, cuKernelGetName 14, LaunchKernel 14, StreamGetDevice 10, EventCreateWithFlags 10 |
| 2 | stage::gather | 4 | 0.49 | 0.27 | 116 (29.1) | 13 / 0.16 | - | 0 | DtoH 0 (0.0); PtoP 3 (0.1) | StreamWaitEvent 20, EventRecord 20, MemcpyAsync 17, EventCreateWithFlags 16, EventDestroy 16 |
| 4 | accum::contribute | 10 | 0.88 | 0.13 | 80 (8.0) | 0 / 0.00 | add_if_n 10 3.75 | 0 | - | StreamWaitEvent 20, StreamGetDevice 10, EventCreateWithFlags 10, cuKernelGetName 10, LaunchKernel 10 |
| 4 | accum::finish | 1 | 21.70 | 21.72 | 375 (375.0) | 146 / 20.20 | sirius::op:: 42 0.16 | 0 | DtoH 109 (2.0); HtoD 219 (4.2); PtoP 766 (18.2) | StreamSynchronize 146, MemcpyBatchAsync 140, cuKernelGetName 42, LaunchKernel 42, MemPoolGetAccess 2 |
| 4 | construction::begin | 2 | 0.23 | 0.24 | 20 (10.0) | 0 / 0.00 | - | 438 | - | StreamCreateWithFlags 4, MallocFromPoolAsync 4, MemsetAsync 4, EventCreateWithFlags 4, EventRecord 4 |
| 4 | replicate_devices | 1 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |
| 12 | accum::contribute | 44 | 2.20 | 0.18 | 352 (8.0) | 0 / 0.00 | add_if_n 44 0.79 | 0 | - | StreamWaitEvent 88, StreamGetDevice 44, EventCreateWithFlags 44, cuKernelGetName 44, LaunchKernel 44 |
| 12 | accum::finish | 1 | 11.48 | 11.48 | 219 (219.0) | 86 / 10.53 | sirius::op:: 24 0.09 | 0 | DtoH 59 (1.1); HtoD 59 (1.7); PtoP 476 (10.5) | StreamSynchronize 86, MemcpyBatchAsync 80, cuKernelGetName 24, LaunchKernel 24, MemPoolGetAccess 2 |
| 12 | construction::begin | 1 | 0.23 | 0.25 | 20 (20.0) | 0 / 0.00 | - | 238 | - | StreamCreateWithFlags 4, MallocFromPoolAsync 4, MemsetAsync 4, EventCreateWithFlags 4, EventRecord 4 |
| 12 | replicate_devices | 1 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |

### DF-site tasks (iteration 2 of each capture; tasks with task wall >= 1 ms or publishing)

| q | task | op | publishing | task wall ms | execute range ms | DF ranges ms | contribute ranges ms (API calls) | insert kernel GPU ms | insert kernels end after contribute returns ms | pre-finish ms (hash path: scatter) | finish chain ms | syncs (ms) in task | HOST/Peer copy MB | post-finish ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
