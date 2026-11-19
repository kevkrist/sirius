| q | pipeline (tip) | role | tip ms on chain (min-max) | DF-off ms | delta ms |
|---|---|---|---|---|---|
| 2 | PARTITION(1) | DF site | 0.3 (0.3-0.3) | 0.1 | +0.2 |
| 2 | PARTITION(9) | DF site | 0.3 (0.3-0.3) | 0.1 | +0.2 |
| 2 | GPU_SCAN(11) -> DYNAMIC_FILTER(12) | probe scan+DF | 21.4 (21.4-26.5) | 19.3 | +2.1 |
| 2 | PARTITION(16) | DF site | 3.3 (3.3-3.3) | 0.1 | +3.2 |
| 2 | PARTITION(19) | DF site | 1.7 (1.6-1.7) | 0.1 | +1.5 |
| 2 | GPU_SCAN(21) -> DYNAMIC_FILTER(22) | probe scan+DF | 45.2 (45.2-45.3) | 44.0 | +1.2 |
| 2 | PARTITION(32) | DF site | 6.6 (4.6-9.2) | 4.9 | +1.8 |
| 3 | PARTITION(2) | DF site | 4.2 (4.2-4.3) | 0.1 | +4.1 |
| 3 | GPU_SCAN(4) -> DYNAMIC_FILTER(5) | probe scan+DF | 84.4 (84.3-84.6) | 88.5 | -4.0 |
| 3 | PARTITION(9) | DF site | 57.2 (57.1-57.3) | 0.2 | +57.0 |
| 3 | GPU_SCAN(11) -> DYNAMIC_FILTER(12) | probe scan+DF | 316.3 (315.9-316.6) | 327.2 | -11.0 |
| 4 | BUILD_FILTER(1) | DF site | 21.2 (21.1-24.0) | no match | +21.2 |
| 4 | GPU_SCAN(4) -> DYNAMIC_FILTER(5) -> FILT | probe scan+DF | 208.3 (208.0-208.5) | 219.9 | -11.6 |
| 5 | PARTITION(4) | DF site | 0.4 (0.3-0.4) | 0.1 | +0.3 |
| 5 | PARTITION(12) | DF site | 0.3 (0.3-0.4) | 0.1 | +0.3 |
| 5 | GPU_SCAN(14) -> DYNAMIC_FILTER(15) | probe scan+DF | 13.0 (6.4-13.2) | 9.6 | +3.4 |
| 5 | PARTITION(19) | DF site | 4.2 (4.2-4.3) | 0.1 | +4.2 |
| 5 | GPU_SCAN(21) -> DYNAMIC_FILTER(22) | probe scan+DF | 85.3 (85.1-103.7) | 83.2 | +2.1 |
| 5 | PARTITION(26) | DF site | 16.8 (16.8-16.9) | 0.2 | +16.6 |
| 5 | GPU_SCAN(28) -> DYNAMIC_FILTER(29) | probe scan+DF | 398.8 (398.6-399.0) | 369.0 | +29.8 |
| 7 | PARTITION(3) | DF site | 0.3 (0.3-0.4) | 0.1 | +0.3 |
| 7 | PARTITION(10) | DF site | 3.9 (3.9-4.0) | 2.1 | +1.9 |
| 7 | PARTITION(15) | DF site | 0.4 (0.3-0.4) | 0.1 | +0.3 |
| 7 | PARTITION(22) | DF site | 1.6 (1.6-1.6) | 0.1 | +1.5 |
| 7 | GPU_SCAN(24) -> DYNAMIC_FILTER(25) | probe scan+DF | 482.7 (482.3-483.3) | 481.7 | +1.0 |
| 7 | PARTITION(29) | DF site | 63.3 (63.1-64.1) | 17.9 | +45.4 |
| 8 | PARTITION(10) | DF site | 2.4 (2.4-2.5) | 0.1 | +2.3 |
| 8 | PARTITION(13) | DF site | 0.4 (0.3-0.4) | 0.1 | +0.3 |
| 8 | PARTITION(21) | DF site | 0.3 (0.3-0.3) | 0.1 | +0.3 |
| 8 | GPU_SCAN(23) -> DYNAMIC_FILTER(24) | probe scan+DF | 6.4 (6.4-6.4) | 3.6 | +2.8 |
| 8 | PARTITION(28) | DF site | 4.2 (4.2-4.2) | 0.1 | +4.1 |
| 8 | GPU_SCAN(30) -> DYNAMIC_FILTER(31) | probe scan+DF | 86.0 (85.9-86.5) | 83.7 | +2.3 |
| 8 | PARTITION(35) | DF site | 35.6 (35.4-35.7) | 0.1 | +35.4 |
| 8 | GPU_SCAN(37) -> DYNAMIC_FILTER(38) | probe scan+DF | 595.5 (593.3-596.0) | 608.1 | -12.7 |
| 9 | PARTITION(13) | DF site | 3.7 (3.6-3.8) | 0.1 | +3.6 |
| 9 | GPU_SCAN(15) -> DYNAMIC_FILTER(16) | probe scan+DF | 45.7 (45.6-45.9) | 44.0 | +1.6 |
| 9 | PARTITION(23) | DF site | 32.0 (31.9-32.0) | 0.1 | +31.8 |
| 9 | GPU_SCAN(25) -> DYNAMIC_FILTER(26) | probe scan+DF | 714.3 (125.5-726.1) | 677.8 | +36.6 |
| 10 | PARTITION(4) | DF site | 22.6 (22.5-22.7) | 1.4 | +21.2 |
| 10 | GPU_SCAN(6) -> DYNAMIC_FILTER(7) | probe scan+DF | 246.8 (245.7-247.6) | 252.4 | -5.6 |
| 10 | PARTITION(15) | DF site | 17.5 (17.3-17.6) | 1.2 | +16.2 |
| 10 | GPU_SCAN(17) -> DYNAMIC_FILTER(18) | probe scan+DF | 145.0 (140.2-147.2) | 146.1 | -1.2 |
| 11 | PARTITION(1) | DF site | 0.4 (0.3-0.5) | 0.1 | +0.3 |
| 11 | PARTITION(8) | DF site | 0.9 (0.9-1.0) | 0.1 | +0.9 |
| 11 | GPU_SCAN(10) -> DYNAMIC_FILTER(11) | probe scan+DF | 52.7 (52.4-54.8) | 49.9 | +2.8 |
| 11 | PARTITION(22) | DF site | 0.3 (0.3-0.3) | 0.1 | +0.2 |
| 11 | PARTITION(29) | DF site | 0.9 (0.9-1.0) | 0.1 | +0.9 |
| 11 | GPU_SCAN(31) -> DYNAMIC_FILTER(32) | probe scan+DF | 55.0 (53.4-56.6) | 51.2 | +3.8 |
| 12 | PARTITION(2) | DF site | 12.7 (12.3-12.8) | 1.1 | +11.6 |
| 12 | GPU_SCAN(4) -> DYNAMIC_FILTER(5) | probe scan+DF | 21.1 (19.1-22.2) | 33.2 | -12.1 |
| 16 | PARTITION(5) | DF site | 8.6 (8.6-8.6) | 0.1 | +8.5 |
| 16 | GPU_SCAN(7) -> DYNAMIC_FILTER(8) | probe scan+DF | 49.9 (49.8-50.0) | 41.5 | +8.4 |
| 17 | PARTITION(1) | DF site | 0.8 (0.7-0.8) | 0.1 | +0.7 |
| 17 | GPU_SCAN(3) -> DYNAMIC_FILTER(4) | probe scan+DF | 454.0 (453.4-462.2) | 452.8 | +1.2 |
| 18 | PARTITION(8) | DF site | 3.1 (1.5-4.8) | 0.6 | +2.5 |
| 18 | PARTITION(18) | DF site | 0.7 (0.6-0.9) | 0.1 | +0.6 |
| 18 | GPU_SCAN(20) -> DYNAMIC_FILTER(21) | probe scan+DF | 127.6 (126.8-127.8) | 126.2 | +1.4 |
| 19 | PARTITION(2) | DF site | 1.3 (1.2-1.3) | 0.1 | +1.2 |
| 19 | GPU_SCAN(4) -> DYNAMIC_FILTER(5) -> FILT | probe scan+DF | 813.6 (812.6-815.3) | 819.8 | -6.2 |
| 20 | PARTITION(1) | DF site | 0.3 (0.3-0.4) | 0.1 | +0.3 |
| 20 | GPU_SCAN(3) -> DYNAMIC_FILTER(4) | probe scan+DF | 13.2 (12.8-13.3) | 10.8 | +2.4 |
| 20 | BUILD_FILTER(8) | DF site | 1.0 (0.9-1.0) | no match | +1.0 |
| 20 | PARTITION(12) | DF site | 1.0 (1.0-1.2) | 0.1 | +0.9 |
| 20 | GPU_SCAN(14) -> DYNAMIC_FILTER(15) | probe scan+DF | 60.3 (49.0-65.7) | 53.3 | +7.0 |
| 20 | PARTITION(25) | DF site | 1.5 (1.4-1.5) | 0.1 | +1.4 |
| 20 | GPU_SCAN(27) -> DYNAMIC_FILTER(28) | probe scan+DF | 538.0 (537.8-543.0) | 536.6 | +1.4 |
| 20 | PARTITION(38) | DF site | 0.1 (0.1-0.1) | 0.6 | -0.5 |
| 21 | PARTITION(1) | DF site | 0.3 (0.3-0.4) | 0.1 | +0.2 |
| 21 | GPU_SCAN(3) -> DYNAMIC_FILTER(4) | probe scan+DF | 14.9 (14.7-15.0) | 9.3 | +5.6 |
| 21 | PARTITION(8) | DF site | 1.0 (1.0-1.0) | 0.1 | +0.9 |
| 21 | PARTITION(11) | DF site | 317.4 (316.7-317.6) | 12.1 | +305.2 |
| 21 | GPU_SCAN(13) -> DYNAMIC_FILTER(14) -> FI | probe scan+DF | 378.5 (374.7-379.2) | 346.4 | +32.1 |
| 21 | PARTITION(27) | DF site | 26.2 (26.1-26.2) | 0.1 | +26.1 |
| 21 | GPU_SCAN(29) -> DYNAMIC_FILTER(30) | probe scan+DF | 375.4 (374.7-380.0) | 364.7 | +10.6 |
| 22 | PARTITION(22) | DF site | 7.3 (7.2-7.4) | 0.2 | +7.1 |
| 22 | GPU_SCAN(24) -> DYNAMIC_FILTER(25) | probe scan+DF | 121.4 (121.3-121.4) | 109.3 | +12.2 |
