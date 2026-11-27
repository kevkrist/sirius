| q | join | site | kinds | n | Publishing Task ms med | site union ms med | pub @ms med | site start -> first primary scan created ms med | consumer scans: min(created - pub) ms / min(first start - pub) ms / tasks started before pub (sum over iters) |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 7 | PARTITION(1) | small_in_list | 4 | 0.2 | 0.2 | 1 | 0.6 | GPU_SCAN(3): +0.3 / +0.3 / 0 |
| 2 | 15 | PARTITION(9) | small_in_list | 4 | 0.2 | 0.2 | 3 | 0.7 | GPU_SCAN(11): +0.4 / +0.4 / 0 |
| 2 | 25 | PARTITION(19) | hash_in_list | 4 | 1.6 | 1.6 | 67 | 3.4 | GPU_SCAN(21): +1.8 / +1.9 / 0 |
| 2 | 28 | PARTITION(16) | hash_in_list | 4 | 3.2 | 3.2 | 31 | 41.5 | GPU_SCAN(21): +38.1 / +38.1 / 0 |
| 2 | 42 | PARTITION(32) | none+none | 4 | 4.6 | 4.6 | 123 | - |  |
| 3 | 8 | PARTITION(2) | bloom | 4 | 4.2 | 4.2 | 14 | 15.7 | GPU_SCAN(4): +11.4 / +11.5 / 0 |
| 3 | 15 | PARTITION(9) | bloom | 4 | 57.2 | 57.3 | 182 | 167.8 | GPU_SCAN(11): +108.4 / +108.6 / 0 |
| 5 | 10 | PARTITION(4) | small_in_list | 4 | 0.3 | 0.3 | 10 | 0.6 | GPU_SCAN(6): +0.2 / +0.3 / 0 |
| 5 | 18 | PARTITION(12) | small_in_list | 4 | 0.3 | 0.3 | 12 | 0.7 | GPU_SCAN(14): +0.4 / +0.4 / 0 |
| 5 | 25 | PARTITION(19) | bloom | 4 | 4.2 | 4.2 | 26 | 53.1 | GPU_SCAN(21): +48.8 / +48.8 / 0 |
| 5 | 32 | PARTITION(26) | bloom | 4 | 16.8 | 16.9 | 198 | 73.0 | GPU_SCAN(28): +52.7 / +52.8 / 0 |
| 7 | 9 | PARTITION(3) | small_in_list | 4 | 0.3 | 0.3 | 2 | 0.7 | GPU_SCAN(5): +0.4 / +0.4 / 0 |
| 7 | 21 | PARTITION(15) | small_in_list | 4 | 0.3 | 0.3 | 18 | 0.6 | GPU_SCAN(17): +0.4 / +0.4 / 0 |
| 7 | 28 | PARTITION(22) | hash_in_list | 4 | 1.5 | 1.5 | 22 | 2.9 | GPU_SCAN(24): +1.4 / +1.4 / 0 |
| 7 | 35 | PARTITION(29) | bloom | 4 | 63.2 | 63.2 | 579 | 119.2 | GPU_SCAN(31): +52.9 / +52.9 / 0 |
| 7 | 38 | PARTITION(10) | bloom | 4 | 3.8 | 3.8 | 12 | 626.0 | GPU_SCAN(17)*: +6.1 / +6.1 / 0; GPU_SCAN(24)*: +10.7 / +10.8 / 0; GPU_SCAN(31): +619.5 / +619.5 / 0 |
| 8 | 19 | PARTITION(13) | small_in_list | 4 | 0.3 | 0.3 | 19 | 0.5 | GPU_SCAN(15): +0.2 / +0.3 / 0 |
| 8 | 27 | PARTITION(21) | small_in_list | 4 | 0.3 | 0.3 | 20 | 0.6 | GPU_SCAN(23): +0.3 / +0.3 / 0 |
| 8 | 34 | PARTITION(28) | bloom | 4 | 4.1 | 4.1 | 33 | 32.9 | GPU_SCAN(30): +28.7 / +28.8 / 0 |
| 8 | 41 | PARTITION(35) | bloom | 4 | 35.6 | 35.7 | 197 | 136.2 | GPU_SCAN(37): +93.8 / +93.9 / 0 |
| 8 | 44 | PARTITION(10) | hash_in_list | 4 | 2.3 | 2.3 | 17 | 282.3 | GPU_SCAN(15)*: +1.9 / +1.9 / 0; GPU_SCAN(23)*: +3.4 / +3.4 / 0; GPU_SCAN(30)*: +44.4 / +44.4 / 0; GPU_SCAN(37): +273.6 / +273.7 / 0 |
| 9 | 19 | PARTITION(13) | bloom | 4 | 3.6 | 3.6 | 356 | 6.2 | GPU_SCAN(15): +2.4 / +2.4 / 0 |
| 9 | 29 | PARTITION(23) | bloom+bloom | 4 | 31.8 | 31.9 | 443 | 83.2 | GPU_SCAN(25): +51.0 / +51.3 / 0 |
| 10 | 10 | PARTITION(4) | bloom | 4 | 22.4 | 22.6 | 107 | 31.7 | GPU_SCAN(6): +9.0 / +9.0 / 0 |
| 10 | 21 | PARTITION(15) | bloom | 4 | 17.4 | 17.4 | 436 | 18.0 | GPU_SCAN(17): +0.6 / +0.7 / 0 |
| 11 | 7 | PARTITION(1) | small_in_list | 4 | 0.3 | 0.3 | 2 | 0.6 | GPU_SCAN(3): +0.3 / +0.4 / 0 |
| 11 | 14 | PARTITION(8) | hash_in_list | 4 | 0.9 | 0.9 | 4 | 1.5 | GPU_SCAN(10): +0.6 / +0.7 / 0 |
| 11 | 28 | PARTITION(22) | small_in_list | 4 | 0.2 | 0.2 | 62 | 0.5 | GPU_SCAN(24): +0.2 / +0.3 / 0 |
| 11 | 35 | PARTITION(29) | hash_in_list | 4 | 0.8 | 0.8 | 64 | 1.5 | GPU_SCAN(31): +0.6 / +0.6 / 0 |
| 12 | 8 | PARTITION(2) | bloom | 4 | 11.2 | 12.2 | 625 | 43.3 | GPU_SCAN(4): +28.7 / +28.7 / 0 |
| 16 | 11 | PARTITION(5) | bloom | 4 | 8.5 | 8.5 | 64 | 76.3 | GPU_SCAN(7): +62.9 / +62.9 / 0 |
| 17 | 7 | PARTITION(1) | hash_in_list | 4 | 0.6 | 0.7 | 10 | 1.1 | GPU_SCAN(3): +0.4 / +0.5 / 0 |
| 18 | 14 | PARTITION(8) | hash_in_list | 4 | 1.0 | 1.5 | 773 | 5.7 | GPU_SCAN(10): +2.2 / +2.2 / 0 |
| 18 | 24 | PARTITION(18) | hash_in_list | 4 | 0.5 | 0.6 | 929 | 2.2 | GPU_SCAN(20): +1.5 / +1.6 / 0 |
| 19 | 9 | PARTITION(2) | hash_in_list | 4 | 1.2 | 1.2 | 45 | 3.2 | GPU_SCAN(4): +2.0 / +2.2 / 0 |
| 20 | 7 | PARTITION(1) | small_in_list | 4 | 0.3 | 0.3 | 2 | 0.6 | GPU_SCAN(3): +0.3 / +0.4 / 0 |
| 20 | 18 | PARTITION(12) | bloom | 4 | 1.0 | 1.0 | 85 | 2.0 | GPU_SCAN(14): +0.8 / +0.8 / 0 |
| 20 | 31 | PARTITION(25) | hash_in_list+hash_in_list | 4 | 1.4 | 1.4 | 144 | 1.9 | GPU_SCAN(27): +0.5 / +0.5 / 0 |
| 20 | 45 | PARTITION(38) | none+none | 4 | 0.0 | 0.0 | 691 | - |  |
| 20 | 49 | BUILD_FILTER(8) | hash_in_list | 4 | 0.9 | 0.9 | 16 | 70.7 | GPU_SCAN(14): +55.9 / +56.0 / 0; GPU_SCAN(27)*: +120.4 / +120.4 / 0 |
| 21 | 7 | PARTITION(1) | small_in_list | 4 | 0.3 | 0.3 | 3 | 0.5 | GPU_SCAN(3): +0.3 / +0.3 / 0 |
| 21 | 18 | PARTITION(11) | bloom | 4 | 317.3 | 317.3 | 362 | 396.5 | GPU_SCAN(13): +74.3 / +74.4 / 0 |
| 21 | 21 | PARTITION(8) | hash_in_list | 4 | 0.9 | 0.9 | 14 | 428.5 | GPU_SCAN(13): +422.9 / +423.0 / 0 |
| 21 | 33 | PARTITION(27) | bloom | 4 | 26.1 | 26.2 | 925 | 52.1 | GPU_SCAN(29): +25.5 / +25.6 / 0 |
| 22 | 28 | PARTITION(22) | bloom | 4 | 7.1 | 7.2 | 146 | 11.0 | GPU_SCAN(24): +3.7 / +3.7 / 0 |

join-instances 180, consumer scan tasks started before publication: 0
