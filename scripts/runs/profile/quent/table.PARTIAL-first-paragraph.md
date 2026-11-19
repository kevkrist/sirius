| q | wall | join | site | path | kinds | bloom MB | tasks | med ms | last ms | sum ms | union ms | resv max ms | pub @ms | consumer scan(s): first prep - pub ms / tasks before pub / out bytes before pub |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 139 | 7 | PARTITION(1) | fwd | small_in_list | 0 | 1 | 0.24 | 0.24 | 0.2 | 0.2 | 0.03 | 1 | GPU_SCAN(3): +0.4 / 0 of 1 / 0MB |
| 2 | 139 | 15 | PARTITION(9) | fwd | small_in_list | 0 | 1 | 0.22 | 0.22 | 0.2 | 0.2 | 0.03 | 3 | GPU_SCAN(11): +0.5 / 0 of 1 / 0MB |
| 2 | 139 | 28 | PARTITION(16) | fwd | hash_in_list | 0 | 1 | 3.24 | 3.24 | 3.2 | 3.2 | 0.02 | 31 | GPU_SCAN(21): +38.6 / 0 of 4 / 0MB |
| 2 | 139 | 25 | PARTITION(19) | fwd | hash_in_list | 0 | 3 | 0.05 | 1.57 | 1.7 | 1.6 | 0.02 | 67 | GPU_SCAN(21): +1.9 / 0 of 4 / 0MB |
| 2 | 139 | 42 | PARTITION(32) | hash | none+none | 0 | 4 | 4.78 | 4.68 | 20.2 | 6.6 | 0.02 | 125 |  |
| 3 | 656 | 8 | PARTITION(2) | fwd | bloom | 60 | 1 | 4.18 | 4.18 | 4.2 | 4.2 | 0.02 | 17 | GPU_SCAN(4): +11.5 / 0 of 8 / 0MB |
| 3 | 656 | 15 | PARTITION(9) | fwd | bloom | 294 | 4 | 5.51 | 57.12 | 72.9 | 57.2 | 0.03 | 184 | GPU_SCAN(11): +111.0 / 0 of 44 / 0MB |
| 4 | 316 | 9 | BUILD_FILTER(1) | hash | bloom | 115 | 10 | 0.64 | 20.97 | 26.4 | 21.3 | 0.05 | 63 | GPU_SCAN(4): +0.1 / 0 of 25 / 0MB |
| 5 | 681 | 10 | PARTITION(4) | fwd | small_in_list | 0 | 1 | 0.30 | 0.30 | 0.3 | 0.3 | 0.02 | 12 | GPU_SCAN(6): +0.3 / 0 of 1 / 0MB |
| 5 | 681 | 18 | PARTITION(12) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.02 | 14 | GPU_SCAN(14): +0.5 / 0 of 1 / 0MB |
| 5 | 681 | 25 | PARTITION(19) | fwd | bloom | 60 | 1 | 4.18 | 4.18 | 4.2 | 4.2 | 0.02 | 35 | GPU_SCAN(21): +49.0 / 0 of 7 / 0MB |
| 5 | 681 | 32 | PARTITION(26) | fwd | bloom | 91 | 4 | 0.53 | 16.73 | 18.1 | 16.8 | 0.03 | 199 | GPU_SCAN(28): +54.5 / 0 of 44 / 0MB |
| 7 | 738 | 9 | PARTITION(3) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.02 | 2 | GPU_SCAN(5): +0.5 / 0 of 1 / 0MB |
| 7 | 738 | 38 | PARTITION(10) | hash | bloom | 24 | 1 | 3.86 | 3.86 | 3.9 | 3.9 | 0.02 | 12 | GPU_SCAN(17)*: +6.0 / 0 of 1 / 0MB; GPU_SCAN(24)*: +10.9 / 0 of 50 / 0MB; GPU_SCAN(31): +626.0 / 0 of 5 / 0MB |
| 7 | 738 | 21 | PARTITION(15) | fwd | small_in_list | 0 | 1 | 0.31 | 0.31 | 0.3 | 0.3 | 0.02 | 18 | GPU_SCAN(17): +0.4 / 0 of 1 / 0MB |
| 7 | 738 | 28 | PARTITION(22) | fwd | hash_in_list | 0 | 1 | 1.54 | 1.54 | 1.5 | 1.5 | 0.02 | 22 | GPU_SCAN(24): +1.5 / 0 of 50 / 0MB |
| 7 | 738 | 35 | PARTITION(29) | hash | bloom | 292 | 4 | 12.30 | 63.24 | 99.6 | 63.3 | 0.02 | 579 | GPU_SCAN(31): +58.4 / 0 of 5 / 0MB |
| 8 | 925 | 44 | PARTITION(10) | fwd | hash_in_list | 0 | 2 | 1.20 | 2.35 | 2.4 | 2.4 | 0.02 | 17 | GPU_SCAN(15)*: +2.1 / 0 of 1 / 0MB; GPU_SCAN(23)*: +3.6 / 0 of 1 / 0MB; GPU_SCAN(30)*: +45.9 / 0 of 7 / 0MB; GPU_SCAN(37): +281.9 / 0 of 50 / 0MB |
| 8 | 925 | 19 | PARTITION(13) | fwd | small_in_list | 0 | 1 | 0.30 | 0.30 | 0.3 | 0.3 | 0.02 | 19 | GPU_SCAN(15): +0.3 / 0 of 1 / 0MB |
| 8 | 925 | 27 | PARTITION(21) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.01 | 20 | GPU_SCAN(23): +0.4 / 0 of 1 / 0MB |
| 8 | 925 | 34 | PARTITION(28) | fwd | bloom | 60 | 1 | 4.14 | 4.14 | 4.1 | 4.1 | 0.02 | 33 | GPU_SCAN(30): +30.0 / 0 of 7 / 0MB |
| 8 | 925 | 41 | PARTITION(35) | fwd | bloom | 182 | 4 | 2.77 | 35.51 | 42.8 | 35.6 | 0.02 | 198 | GPU_SCAN(37): +101.9 / 0 of 50 / 0MB |
| 9 | 1393 | 19 | PARTITION(13) | fwd | bloom | 22 | 3 | 0.15 | 3.58 | 3.8 | 3.7 | 0.02 | 348 | GPU_SCAN(15): +2.6 / 0 of 4 / 0MB |
| 9 | 1393 | 29 | PARTITION(23) | fwd | bloom+bloom | 174 | 4 | 1.15 | 31.86 | 34.6 | 31.9 | 0.02 | 435 | GPU_SCAN(25): +51.7 / 0 of 62 / 0MB |
| 10 | 840 | 10 | PARTITION(4) | hash | bloom | 115 | 7 | 2.23 | 22.53 | 33.3 | 22.7 | 0.02 | 107 | GPU_SCAN(6): +8.8 / 0 of 45 / 0MB |
| 10 | 840 | 21 | PARTITION(15) | hash | bloom | 89 | 4 | 1.56 | 17.37 | 22.0 | 17.4 | 0.03 | 437 | GPU_SCAN(17): +0.7 / 0 of 7 / 0MB |
| 11 | 170 | 7 | PARTITION(1) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.03 | 2 | GPU_SCAN(3): +0.4 / 0 of 1 / 0MB |
| 11 | 170 | 14 | PARTITION(8) | fwd | hash_in_list | 0 | 1 | 0.86 | 0.86 | 0.9 | 0.9 | 0.02 | 4 | GPU_SCAN(10): +0.7 / 0 of 5 / 0MB |
| 11 | 170 | 28 | PARTITION(22) | fwd | small_in_list | 0 | 1 | 0.22 | 0.22 | 0.2 | 0.2 | 0.02 | 63 | GPU_SCAN(24): +0.3 / 0 of 1 / 0MB |
| 11 | 170 | 35 | PARTITION(29) | fwd | hash_in_list | 0 | 1 | 0.87 | 0.87 | 0.9 | 0.9 | 0.03 | 65 | GPU_SCAN(31): +0.7 / 0 of 5 / 0MB |
| 12 | 693 | 8 | PARTITION(2) | fwd | bloom | 62 | 44 | 0.06 | 11.22 | 13.8 | 12.1 | 0.04 | 625 | GPU_SCAN(4): +30.9 / 0 of 8 / 0MB |
| 16 | 309 | 11 | PARTITION(5) | fwd | bloom | 59 | 3 | 0.30 | 8.49 | 9.0 | 8.5 | 0.04 | 64 | GPU_SCAN(7): +73.4 / 0 of 2 / 0MB |
| 17 | 485 | 7 | PARTITION(1) | fwd | hash_in_list | 0 | 2 | 0.37 | 0.70 | 0.7 | 0.7 | 0.02 | 10 | GPU_SCAN(3): +0.5 / 0 of 31 / 0MB |
| 18 | 1078 | 14 | PARTITION(8) | fwd | hash_in_list | 0 | 36 | 0.04 | 1.03 | 3.2 | 1.5 | 0.03 | 788 | GPU_SCAN(10): +2.5 / 0 of 10 / 0MB |
| 18 | 1078 | 24 | PARTITION(18) | fwd | hash_in_list | 0 | 4 | 0.05 | 0.57 | 0.7 | 0.6 | 0.02 | 941 | GPU_SCAN(20): +1.7 / 0 of 25 / 0MB |
| 19 | 879 | 9 | PARTITION(2) | fwd | hash_in_list | 0 | 2 | 0.63 | 1.20 | 1.3 | 1.2 | 0.02 | 54 | GPU_SCAN(4): +2.2 / 0 of 81 / 0MB |
| 20 | 708 | 7 | PARTITION(1) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.02 | 3 | GPU_SCAN(3): +0.4 / 0 of 1 / 0MB |
| 20 | 708 | 49 | BUILD_FILTER(8) | hash | hash_in_list | 0 | 1 | 0.91 | 0.91 | 0.9 | 0.9 | 0.02 | 20 | GPU_SCAN(14): +56.9 / 0 of 3 / 0MB; GPU_SCAN(27)*: +126.9 / 0 of 31 / 0MB |
| 20 | 708 | 18 | PARTITION(12) | fwd | bloom | 4 | 3 | 0.07 | 0.96 | 1.1 | 1.0 | 0.02 | 76 | GPU_SCAN(14): +0.9 / 0 of 3 / 0MB |
| 20 | 708 | 31 | PARTITION(25) | fwd | hash_in_list+hash_in_list | 0 | 1 | 1.42 | 1.42 | 1.4 | 1.4 | 0.02 | 146 | GPU_SCAN(27): +0.7 / 0 of 31 / 0MB |
| 20 | 708 | 45 | PARTITION(38) | fwd | none+none | 0 | 1 | 0.03 | 0.03 | 0.0 | 0.0 | 0.02 | 692 |  |
| 21 | 1437 | 7 | PARTITION(1) | fwd | small_in_list | 0 | 1 | 0.26 | 0.26 | 0.3 | 0.3 | 0.03 | 4 | GPU_SCAN(3): +0.3 / 0 of 1 / 0MB |
| 21 | 1437 | 21 | PARTITION(8) | fwd | hash_in_list | 0 | 1 | 0.93 | 0.93 | 0.9 | 0.9 | 0.02 | 21 | GPU_SCAN(13): +424.0 / 0 of 31 / 0MB |
| 21 | 1437 | 18 | PARTITION(11) | hash | bloom | 1462 | 5 | 31.80 | 317.30 | 474.3 | 317.3 | 0.03 | 369 | GPU_SCAN(13): +76.0 / 0 of 31 / 0MB |
| 21 | 1437 | 33 | PARTITION(27) | fwd | bloom | 139 | 4 | 1.58 | 26.09 | 30.8 | 26.1 | 0.03 | 929 | GPU_SCAN(29): +28.8 / 0 of 31 / 0MB |
| 22 | 332 | 28 | PARTITION(22) | fwd | bloom | 38 | 4 | 0.13 | 7.15 | 7.6 | 7.2 | 0.04 | 147 | GPU_SCAN(24): +4.1 / 0 of 2 / 6000MB |

