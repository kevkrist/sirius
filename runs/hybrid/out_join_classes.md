| q | join | type | layout (run time) | class | build rows | build MB | filter kind(s) | filter MB / keys | pushed |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 7 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 1 | 0 | small_in_list |  1 keys | 1 |
| 2 | 15 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 5 | 0 | small_in_list |  5 keys | 1 |
| 2 | 28 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 1999643 | 298 | hash_in_list |  1999643 keys | 1 |
| 2 | 25 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 799927 | 17 | hash_in_list |  799927 keys | 1 |
| 2 | 42 | INNER | 4p hash MIXED_JOIN | PARTITION (multi-partition) | 638799 | 114 | none+none | - | 0/0 |
| 3 | 8 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 30003569 | 114 | bloom | 57 MB | 1 |
| 3 | 15 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 147060214 | 2104 | bloom | 280 MB | 1 |
| 4 | 9 | RIGHT_SEMI | 4p hash STANDARD | BUILD_FILTER | 149850442 | 1143 | bloom | 109 MB | 1 |
| 5 | 10 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 1 | 0 | small_in_list |  1 keys | 1 |
| 5 | 18 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 5 | 0 | small_in_list |  5 keys | 1 |
| 5 | 25 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 29997971 | 572 | bloom | 57 MB | 1 |
| 5 | 32 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 45508350 | 1042 | bloom | 87 MB | 1 |
| 7 | 9 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 2 | 0 | small_in_list |  2 keys | 1 |
| 7 | 38 | INNER | 4p hash BUILD_PROBE | PARTITION (multi-partition) | 12000338 | 195 | bloom | 23 MB | 1 |
| 7 | 21 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 2 | 0 | small_in_list |  2 keys | 1 |
| 7 | 28 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 800280 | 13 | hash_in_list |  800280 keys | 1 |
| 7 | 35 | INNER | 4p hash BUILD_PROBE | PARTITION (multi-partition) | 145884740 | 4312 | bloom | 278 MB | 1 |
| 8 | 44 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 1332415 | 5 | hash_in_list |  1332415 keys | 1 |
| 8 | 19 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 1 | 0 | small_in_list |  1 keys | 1 |
| 8 | 27 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 5 | 0 | small_in_list |  5 keys | 1 |
| 8 | 34 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 30002171 | 343 | bloom | 57 MB | 1 |
| 8 | 41 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 91139462 | 1912 | bloom | 174 MB | 1 |
| 9 | 19 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 10870459 | 41 | bloom | 21 MB | 1 |
| 9 | 29 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 43481836 | 957 | bloom+bloom | 166 MB | 1/1 |
| 10 | 10 | INNER | 4p hash BUILD_PROBE | PARTITION (multi-partition) | 57360614 | 656 | bloom | 109 MB | 1 |
| 10 | 21 | INNER | 4p hash BUILD_PROBE | PARTITION (multi-partition) | 44529913 | 849 | bloom | 85 MB | 1 |
| 11 | 7 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 1 | 0 | small_in_list |  1 keys | 1 |
| 11 | 14 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 399948 | 5 | hash_in_list |  399948 keys | 1 |
| 11 | 28 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 1 | 0 | small_in_list |  1 keys | 1 |
| 11 | 35 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 399948 | 5 | hash_in_list |  399948 keys | 1 |
| 12 | 8 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 31165956 | 535 | bloom | 59 MB | 1 |
| 16 | 11 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 29700215 | 1180 | bloom | 57 MB | 1 |
| 17 | 7 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 200585 | 1 | hash_in_list |  200585 keys | 1 |
| 18 | 14 | SEMI | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 63430 | 0 | hash_in_list |  63430 keys | 1 |
| 18 | 24 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 63430 | 2 | hash_in_list |  63430 keys | 1 |
| 19 | 9 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 480774 | 13 | hash_in_list |  480774 keys | 1 |
| 20 | 7 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 1 | 0 | small_in_list |  1 keys | 1 |
| 20 | 49 | RIGHT_SEMI | 1p hash STANDARD | BUILD_FILTER | 131751 | 1 | hash_in_list |  399302 keys | 2 |
| 20 | 18 | SEMI | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 2174140 | 8 | bloom | 4 MB | 1 |
| 20 | 31 | SEMI | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 347054 | 3 | hash_in_list+hash_in_list |  694108 keys | 1/1 |
| 20 | 45 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 132209 | 3 | none+none | - | 0/0 |
| 21 | 7 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 1 | 0 | small_in_list |  1 keys | 1 |
| 21 | 21 | INNER | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 399867 | 11 | hash_in_list |  399867 keys | 1 |
| 21 | 18 | INNER | 4p hash BUILD_PROBE | PARTITION (multi-partition) | 730806711 | 5576 | bloom | 1394 MB | 1 |
| 21 | 33 | SEMI | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 69665236 | 532 | bloom | 133 MB | 1 |
| 22 | 28 | SEMI | 4p broadcast BUILD_PROBE | ONE-SHOT (arrives whole) | 19092648 | 73 | bloom | 36 MB | 1 |

{'ONE-SHOT (arrives whole)': 38, 'ONE-SHOT (arrives whole) bloomMB': 1188.9346618652344, 'PARTITION (multi-partition)': 6, 'PARTITION (multi-partition) bloomMB': 1889.3859252929688, 'BUILD_FILTER': 2, 'BUILD_FILTER bloomMB': 109.39898681640625}
