All are final-only diagnostics; no comparative baseline is invented. Each contributor inserts 4,096 rows/key. Bytes and workspace charges are distinct from allocator committed peaks. Raw measured intervals and records are in builder-raw-summary.json and the complete native archives.

| Campaign | Workload | Root | K | B/key bytes | C bytes | Contributors | Transport | Measured samples | Median ms | p90 ms | Input bytes | Ready bytes | Logical edge bytes | Aligned scratch bytes | Physical HOST bytes | D2H/H2D bytes | Statuses |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| builder-screen | builder-k1-b1048832-r3 | 3 | 1 | 1048832 | 1048832 | 4 | "direct" | 3 | 0.312674 | 0.315178 | 4195328 | 4195328 | 6292992 | 3146496 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b67109120-r0 | 0 | 1 | 67109120 | 8388608 | 4 | "direct" | 3 | 9.114460 | 9.167029 | 268436480 | 268436480 | 402654720 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b67109120-r1 | 1 | 1 | 67109120 | 8388608 | 4 | "direct" | 3 | 9.087890 | 9.103282 | 268436480 | 268436480 | 402654720 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b67109120-r2 | 2 | 1 | 67109120 | 8388608 | 4 | "direct" | 3 | 9.282252 | 9.283356 | 268436480 | 268436480 | 402654720 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b67109120-r3 | 3 | 1 | 67109120 | 8388608 | 4 | "direct" | 3 | 9.290062 | 9.290198 | 268436480 | 268436480 | 402654720 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k1-b268435200-r3 | 3 | 1 | 268435200 | 8388608 | 4 | "direct" | 3 | 34.544858 | 35.929970 | 1073740800 | 1073740800 | 1610611200 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-k2-b67109120-r3 | 3 | 2 | 67109120 | 8388608 | 4 | "direct" | 3 | 18.489313 | 18.566131 | 536872960 | 536872960 | 805309440 | 50331648 | 0 | 0/0 | {'ok': 3} |
| builder-screen | builder-force-host | 3 | 2 | 67109120 | 8388608 | 4 | "host" | 3 | 52.938312 | 52.980431 | 536872960 | 536872960 | 805309440 | 8388608 | 8388608 | 536872960/805309440 | {'ok': 3} |
| builder-heldout | builder-k1-b8388864-r0 | 0 | 1 | 8388864 | 2097152 | 4 | "direct" | 30 | 1.407727 | 1.417512 | 33555456 | 33555456 | 50333184 | 12582912 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k1-b8388864-r3 | 3 | 1 | 8388864 | 2097152 | 4 | "direct" | 30 | 1.403132 | 1.418433 | 33555456 | 33555456 | 50333184 | 12582912 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k1-b134217984-r0 | 0 | 1 | 134217984 | 8388608 | 4 | "direct" | 30 | 17.158968 | 17.183079 | 536871936 | 536871936 | 805307904 | 50331648 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k1-b134217984-r3 | 3 | 1 | 134217984 | 8388608 | 4 | "direct" | 30 | 17.544483 | 17.593310 | 536871936 | 536871936 | 805307904 | 50331648 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k2-b8388864-r0 | 0 | 2 | 8388864 | 2097152 | 4 | "direct" | 30 | 2.508881 | 2.535594 | 67110912 | 67110912 | 100666368 | 12582912 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k2-b8388864-r3 | 3 | 2 | 8388864 | 2097152 | 4 | "direct" | 30 | 2.516755 | 2.534499 | 67110912 | 67110912 | 100666368 | 12582912 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k2-b134217472-r0 | 0 | 2 | 134217472 | 8388608 | 4 | "direct" | 30 | 33.180427 | 33.270892 | 1073739776 | 1073739776 | 1610609664 | 50331648 | 0 | 0/0 | {'ok': 30} |
| builder-heldout | builder-k2-b134217472-r3 | 3 | 2 | 134217472 | 8388608 | 4 | "direct" | 30 | 33.933551 | 34.074693 | 1073739776 | 1073739776 | 1610609664 | 50331648 | 0 | 0/0 | {'ok': 30} |
