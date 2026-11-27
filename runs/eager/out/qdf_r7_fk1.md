| q | join | site | kinds | n | Publishing Task ms med | site union ms med | pub @ms med | site start -> first primary scan created ms med | consumer scans: min(created - pub) ms / min(first start - pub) ms / tasks started before pub (sum over iters) |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 7 | PARTITION(1) | small_in_list | 8 | 0.2 | 0.2 | 1 | 0.4 | GPU_SCAN(3): +0.1 / +0.1 / 0 |
| 2 | 15 | PARTITION(9) | small_in_list | 8 | 0.2 | 0.2 | 3 | 0.4 | GPU_SCAN(11): +0.1 / +0.1 / 0 |
| 2 | 25 | PARTITION(19) | hash_in_list | 8 | 1.9 | 2.0 | 42 | 2.1 | GPU_SCAN(21): +0.1 / +0.2 / 0 |
| 2 | 28 | PARTITION(16) | hash_in_list | 8 | 3.2 | 3.2 | 30 | 15.3 | GPU_SCAN(21): +11.6 / +11.6 / 0 |
| 2 | 42 | PARTITION(32) | none+none | 8 | 4.5 | 7.2 | 100 | - |  |
| 3 | 8 | PARTITION(2) | bloom | 8 | 4.2 | 4.2 | 14 | 4.3 | GPU_SCAN(4): +0.1 / +0.2 / 0 |
| 3 | 15 | PARTITION(9) | bloom | 8 | 57.2 | 57.2 | 174 | 57.4 | GPU_SCAN(11): +0.1 / +0.2 / 0 |
| 5 | 10 | PARTITION(4) | small_in_list | 8 | 0.4 | 0.4 | 4 | 0.6 | GPU_SCAN(6): +0.1 / +0.2 / 0 |
| 5 | 18 | PARTITION(12) | small_in_list | 8 | 0.3 | 0.3 | 7 | 0.5 | GPU_SCAN(14): +0.1 / +0.2 / 0 |
| 5 | 25 | PARTITION(19) | bloom | 8 | 4.2 | 4.2 | 21 | 4.3 | GPU_SCAN(21): +0.1 / +0.1 / 0 |
| 5 | 32 | PARTITION(26) | bloom | 8 | 16.7 | 16.8 | 173 | 16.9 | GPU_SCAN(28): +0.1 / +0.2 / 0 |
| 7 | 9 | PARTITION(3) | small_in_list | 8 | 0.3 | 0.3 | 2 | 0.5 | GPU_SCAN(5): +0.1 / +0.2 / 0 |
| 7 | 21 | PARTITION(15) | small_in_list | 8 | 0.3 | 0.3 | 14 | 0.5 | GPU_SCAN(17): +0.1 / +0.2 / 0 |
| 7 | 28 | PARTITION(22) | hash_in_list | 8 | 1.7 | 1.7 | 17 | 1.8 | GPU_SCAN(24): +0.1 / +0.2 / 0 |
| 7 | 35 | PARTITION(29) | bloom | 8 | 63.2 | 63.2 | 575 | 63.4 | GPU_SCAN(31): +0.1 / +0.1 / 0 |
| 7 | 38 | PARTITION(10) | bloom | 8 | 3.8 | 3.8 | 12 | 566.6 | GPU_SCAN(17)*: +1.4 / +1.5 / 0; GPU_SCAN(24)*: +5.0 / +5.0 / 0; GPU_SCAN(31): +562.3 / +562.4 / 0 |
| 8 | 19 | PARTITION(13) | small_in_list | 8 | 0.3 | 0.3 | 16 | 0.4 | GPU_SCAN(15): +0.1 / +0.1 / 0 |
| 8 | 27 | PARTITION(21) | small_in_list | 8 | 0.3 | 0.3 | 17 | 0.4 | GPU_SCAN(23): +0.1 / +0.1 / 0 |
| 8 | 34 | PARTITION(28) | bloom | 8 | 4.2 | 4.2 | 29 | 4.3 | GPU_SCAN(30): +0.1 / +0.1 / 0 |
| 8 | 41 | PARTITION(35) | bloom | 8 | 35.6 | 35.7 | 175 | 35.8 | GPU_SCAN(37): +0.1 / +0.1 / 0 |
| 8 | 44 | PARTITION(10) | hash_in_list | 8 | 2.3 | 2.3 | 14 | 163.6 | GPU_SCAN(15)*: +1.3 / +1.3 / 0; GPU_SCAN(23)*: +2.6 / +2.7 / 0; GPU_SCAN(30)*: +15.0 / +15.0 / 0; GPU_SCAN(37): +160.9 / +160.9 / 0 |
| 9 | 19 | PARTITION(13) | bloom | 8 | 41.5 | 41.5 | 256 | 47.9 | GPU_SCAN(15): +0.1 / +0.2 / 0 |
| 9 | 29 | PARTITION(23) | bloom+bloom | 8 | 39.7 | 39.8 | 357 | 39.9 | GPU_SCAN(25): +0.1 / +0.1 / 0 |
| 10 | 10 | PARTITION(4) | bloom | 8 | 22.2 | 22.3 | 106 | 22.4 | GPU_SCAN(6): +0.1 / +0.1 / 0 |
| 10 | 21 | PARTITION(15) | bloom | 8 | 17.4 | 17.4 | 436 | 17.5 | GPU_SCAN(17): +0.1 / +0.2 / 0 |
| 11 | 7 | PARTITION(1) | small_in_list | 8 | 0.3 | 0.3 | 2 | 0.4 | GPU_SCAN(3): +0.1 / +0.2 / 0 |
| 11 | 14 | PARTITION(8) | hash_in_list | 8 | 0.9 | 0.9 | 4 | 1.0 | GPU_SCAN(10): +0.1 / +0.2 / 0 |
| 11 | 28 | PARTITION(22) | small_in_list | 8 | 0.2 | 0.2 | 61 | 0.4 | GPU_SCAN(24): +0.1 / +0.1 / 0 |
| 11 | 35 | PARTITION(29) | hash_in_list | 8 | 0.9 | 0.9 | 64 | 1.0 | GPU_SCAN(31): +0.1 / +0.2 / 0 |
| 12 | 8 | PARTITION(2) | bloom | 8 | 11.2 | 12.1 | 625 | 13.0 | GPU_SCAN(4): +0.2 / +0.3 / 0 |
| 16 | 11 | PARTITION(5) | bloom | 8 | 8.5 | 8.5 | 64 | 8.6 | GPU_SCAN(7): +0.0 / +0.1 / 0 |
| 17 | 7 | PARTITION(1) | hash_in_list | 8 | 0.7 | 0.7 | 10 | 0.8 | GPU_SCAN(3): +0.1 / +0.2 / 0 |
| 18 | 14 | PARTITION(8) | hash_in_list | 8 | 1.0 | 1.5 | 697 | 3.5 | GPU_SCAN(10): +0.3 / +0.4 / 0 |
| 18 | 24 | PARTITION(18) | hash_in_list | 8 | 0.5 | 0.6 | 850 | 0.9 | GPU_SCAN(20): +0.2 / +0.2 / 0 |
| 19 | 9 | PARTITION(2) | hash_in_list | 8 | 1.2 | 1.2 | 45 | 1.4 | GPU_SCAN(4): +0.2 / +0.4 / 0 |
| 20 | 7 | PARTITION(1) | small_in_list | 8 | 0.3 | 0.3 | 3 | 0.5 | GPU_SCAN(3): +0.1 / +0.2 / 0 |
| 20 | 18 | PARTITION(12) | bloom | 8 | 0.9 | 1.0 | 85 | 1.2 | GPU_SCAN(14): +0.1 / +0.1 / 0 |
| 20 | 31 | PARTITION(25) | hash_in_list+hash_in_list | 8 | 1.4 | 1.4 | 144 | 1.6 | GPU_SCAN(27): +0.1 / +0.2 / 0 |
| 20 | 45 | PARTITION(38) | none+none | 8 | 0.0 | 0.0 | 691 | - |  |
| 20 | 49 | BUILD_FILTER(8) | hash_in_list | 8 | 0.9 | 0.9 | 16 | 70.1 | GPU_SCAN(14): +55.0 / +55.1 / 0; GPU_SCAN(27)*: +119.2 / +119.3 / 0 |
| 21 | 7 | PARTITION(1) | small_in_list | 8 | 0.3 | 0.3 | 3 | 0.5 | GPU_SCAN(3): +0.2 / +0.2 / 0 |
| 21 | 18 | PARTITION(11) | bloom | 8 | 317.1 | 317.2 | 361 | 317.4 | GPU_SCAN(13): +0.1 / +0.2 / 0 |
| 21 | 21 | PARTITION(8) | hash_in_list | 8 | 0.9 | 0.9 | 14 | 348.2 | GPU_SCAN(13): +335.1 / +335.1 / 0 |
| 21 | 33 | PARTITION(27) | bloom | 8 | 26.1 | 26.1 | 880 | 26.2 | GPU_SCAN(29): +0.1 / +0.2 / 0 |
| 22 | 28 | PARTITION(22) | bloom | 8 | 7.2 | 7.2 | 146 | 7.3 | GPU_SCAN(24): +0.1 / +0.1 / 0 |

join-instances 360, consumer scan tasks started before publication: 0
