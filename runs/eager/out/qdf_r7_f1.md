| q | join | site | kinds | n | Publishing Task ms med | site union ms med | pub @ms med | site start -> first primary scan created ms med | consumer scans: min(created - pub) ms / min(first start - pub) ms / tasks started before pub (sum over iters) |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 7 | PARTITION(1) | small_in_list | 8 | 0.2 | 0.2 | 1 | 0.4 | GPU_SCAN(3): +0.1 / +0.2 / 0 |
| 2 | 15 | PARTITION(9) | small_in_list | 8 | 0.2 | 0.2 | 3 | 0.4 | GPU_SCAN(11): +0.1 / +0.1 / 0 |
| 2 | 25 | PARTITION(19) | hash_in_list | 8 | 2.0 | 2.1 | 42 | 2.3 | GPU_SCAN(21): +0.1 / +0.2 / 0 |
| 2 | 28 | PARTITION(16) | hash_in_list | 8 | 3.2 | 3.2 | 30 | 15.3 | GPU_SCAN(21): +11.3 / +11.4 / 0 |
| 2 | 42 | PARTITION(32) | none+none | 8 | 4.6 | 4.7 | 98 | - |  |
| 3 | 8 | PARTITION(2) | bloom | 8 | 4.2 | 4.2 | 14 | 4.3 | GPU_SCAN(4): +0.1 / +0.2 / 0 |
| 3 | 15 | PARTITION(9) | bloom | 8 | 57.1 | 57.2 | 172 | 57.3 | GPU_SCAN(11): +0.1 / +0.2 / 0 |
| 4 | 9 | BUILD_FILTER(1) | bloom | 4 | 21.1 | 21.6 | 63 | 21.6 | GPU_SCAN(4): +0.0 / +0.1 / 0 |
| 5 | 10 | PARTITION(4) | small_in_list | 8 | 0.4 | 0.4 | 4 | 0.6 | GPU_SCAN(6): +0.2 / +0.2 / 0 |
| 5 | 18 | PARTITION(12) | small_in_list | 8 | 0.3 | 0.3 | 7 | 0.5 | GPU_SCAN(14): +0.1 / +0.1 / 0 |
| 5 | 25 | PARTITION(19) | bloom | 8 | 4.2 | 4.2 | 21 | 4.3 | GPU_SCAN(21): +0.1 / +0.1 / 0 |
| 5 | 32 | PARTITION(26) | bloom | 8 | 16.7 | 16.8 | 174 | 17.0 | GPU_SCAN(28): +0.1 / +0.2 / 0 |
| 7 | 9 | PARTITION(3) | small_in_list | 8 | 0.3 | 0.3 | 2 | 0.5 | GPU_SCAN(5): +0.1 / +0.1 / 0 |
| 7 | 21 | PARTITION(15) | small_in_list | 8 | 0.3 | 0.3 | 14 | 0.5 | GPU_SCAN(17): +0.1 / +0.1 / 0 |
| 7 | 28 | PARTITION(22) | hash_in_list | 8 | 1.6 | 1.6 | 17 | 1.8 | GPU_SCAN(24): +0.1 / +0.2 / 0 |
| 7 | 35 | PARTITION(29) | bloom | 8 | 63.2 | 63.2 | 573 | 63.3 | GPU_SCAN(31): +0.1 / +0.2 / 0 |
| 7 | 38 | PARTITION(10) | bloom | 8 | 3.8 | 3.8 | 12 | 564.9 | GPU_SCAN(17)*: +1.4 / +1.5 / 0; GPU_SCAN(24)*: +5.3 / +5.4 / 0; GPU_SCAN(31): +560.0 / +560.0 / 0 |
| 8 | 19 | PARTITION(13) | small_in_list | 8 | 0.3 | 0.3 | 16 | 0.5 | GPU_SCAN(15): +0.1 / +0.1 / 0 |
| 8 | 27 | PARTITION(21) | small_in_list | 8 | 0.3 | 0.3 | 17 | 0.4 | GPU_SCAN(23): +0.1 / +0.1 / 0 |
| 8 | 34 | PARTITION(28) | bloom | 8 | 4.2 | 4.2 | 29 | 4.3 | GPU_SCAN(30): +0.1 / +0.1 / 0 |
| 8 | 41 | PARTITION(35) | bloom | 8 | 35.6 | 35.7 | 179 | 35.8 | GPU_SCAN(37): +0.1 / +0.1 / 0 |
| 8 | 44 | PARTITION(10) | hash_in_list | 8 | 2.3 | 2.3 | 14 | 166.8 | GPU_SCAN(15)*: +1.4 / +1.4 / 0; GPU_SCAN(23)*: +2.7 / +2.7 / 0; GPU_SCAN(30)*: +15.0 / +15.0 / 0; GPU_SCAN(37): +160.5 / +160.5 / 0 |
| 9 | 19 | PARTITION(13) | bloom | 8 | 38.1 | 38.1 | 256 | 39.2 | GPU_SCAN(15): +0.1 / +0.2 / 0 |
| 9 | 29 | PARTITION(23) | bloom+bloom | 8 | 39.5 | 39.6 | 364 | 39.7 | GPU_SCAN(25): +0.1 / +0.2 / 0 |
| 10 | 10 | PARTITION(4) | bloom | 8 | 22.5 | 22.7 | 106 | 22.8 | GPU_SCAN(6): +0.1 / +0.1 / 0 |
| 10 | 21 | PARTITION(15) | bloom | 8 | 17.3 | 17.4 | 430 | 17.5 | GPU_SCAN(17): +0.1 / +0.2 / 0 |
| 11 | 7 | PARTITION(1) | small_in_list | 8 | 0.3 | 0.3 | 2 | 0.4 | GPU_SCAN(3): +0.1 / +0.2 / 0 |
| 11 | 14 | PARTITION(8) | hash_in_list | 8 | 0.9 | 0.9 | 4 | 1.0 | GPU_SCAN(10): +0.1 / +0.1 / 0 |
| 11 | 28 | PARTITION(22) | small_in_list | 8 | 0.2 | 0.2 | 61 | 0.3 | GPU_SCAN(24): +0.1 / +0.1 / 0 |
| 11 | 35 | PARTITION(29) | hash_in_list | 8 | 0.9 | 0.9 | 63 | 1.0 | GPU_SCAN(31): +0.1 / +0.1 / 0 |
| 12 | 8 | PARTITION(2) | bloom | 8 | 11.2 | 12.1 | 626 | 13.0 | GPU_SCAN(4): +0.1 / +0.3 / 0 |
| 16 | 11 | PARTITION(5) | bloom | 8 | 8.5 | 8.5 | 64 | 8.6 | GPU_SCAN(7): +0.0 / +0.1 / 0 |
| 17 | 7 | PARTITION(1) | hash_in_list | 8 | 0.6 | 0.7 | 10 | 0.8 | GPU_SCAN(3): +0.1 / +0.2 / 0 |
| 18 | 14 | PARTITION(8) | hash_in_list | 8 | 1.0 | 1.4 | 702 | 4.8 | GPU_SCAN(10): +0.2 / +0.4 / 0 |
| 18 | 24 | PARTITION(18) | hash_in_list | 8 | 0.5 | 0.6 | 853 | 0.8 | GPU_SCAN(20): +0.2 / +0.2 / 0 |
| 19 | 9 | PARTITION(2) | hash_in_list | 8 | 1.2 | 1.2 | 45 | 1.4 | GPU_SCAN(4): +0.2 / +0.4 / 0 |
| 20 | 7 | PARTITION(1) | small_in_list | 8 | 0.3 | 0.3 | 3 | 0.4 | GPU_SCAN(3): +0.1 / +0.2 / 0 |
| 20 | 18 | PARTITION(12) | bloom | 8 | 1.0 | 1.0 | 84 | 1.2 | GPU_SCAN(14): +0.1 / +0.1 / 0 |
| 20 | 31 | PARTITION(25) | hash_in_list+hash_in_list | 8 | 1.4 | 1.4 | 143 | 1.6 | GPU_SCAN(27): +0.1 / +0.2 / 0 |
| 20 | 45 | PARTITION(38) | none+none | 8 | 0.0 | 0.0 | 689 | - |  |
| 20 | 49 | BUILD_FILTER(8) | hash_in_list | 8 | 0.9 | 0.9 | 16 | 69.1 | GPU_SCAN(14): +54.7 / +54.7 / 0; GPU_SCAN(27)*: +118.8 / +118.9 / 0 |
| 21 | 7 | PARTITION(1) | small_in_list | 8 | 0.3 | 0.3 | 3 | 0.4 | GPU_SCAN(3): +0.2 / +0.2 / 0 |
| 21 | 18 | PARTITION(11) | bloom | 8 | 317.0 | 317.2 | 362 | 317.3 | GPU_SCAN(13): +0.1 / +0.1 / 0 |
| 21 | 21 | PARTITION(8) | hash_in_list | 8 | 0.9 | 0.9 | 14 | 348.0 | GPU_SCAN(13): +335.0 / +335.0 / 0 |
| 21 | 33 | PARTITION(27) | bloom | 8 | 26.1 | 26.1 | 879 | 26.2 | GPU_SCAN(29): +0.1 / +0.2 / 0 |
| 22 | 28 | PARTITION(22) | bloom | 8 | 7.1 | 7.2 | 146 | 7.3 | GPU_SCAN(24): +0.1 / +0.1 / 0 |

join-instances 364, consumer scan tasks started before publication: 0
