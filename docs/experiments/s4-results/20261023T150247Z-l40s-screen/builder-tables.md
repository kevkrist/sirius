All byte columns are logical quantities or checked-out workspace charges as labeled. Each contributor inserts 4,096 rows/key; four contributors and four replicas are used. Builder returns ready filters and has zero channel-published bytes. Timing is host publication preparation through completion.

| Workload | Root | Contributors | K | B/key bytes | C bytes | Transport | Input bytes | Ready bytes | Logical edges bytes | Scratch requested/aligned bytes | Physical HOST bytes | D2H/H2D bytes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| builder-k1-b1048832-r3 | 3 | 4 | 1 | 1048832 | 1048832 | "direct" | 4195328 | 4195328 | 6292992 | 3146496/3146496 | 0 | 0/0 |
| builder-k1-b67109120-r0 | 0 | 4 | 1 | 67109120 | 8388608 | "direct" | 268436480 | 268436480 | 402654720 | 50331648/50331648 | 0 | 0/0 |
| builder-k1-b67109120-r1 | 1 | 4 | 1 | 67109120 | 8388608 | "direct" | 268436480 | 268436480 | 402654720 | 50331648/50331648 | 0 | 0/0 |
| builder-k1-b67109120-r2 | 2 | 4 | 1 | 67109120 | 8388608 | "direct" | 268436480 | 268436480 | 402654720 | 50331648/50331648 | 0 | 0/0 |
| builder-k1-b67109120-r3 | 3 | 4 | 1 | 67109120 | 8388608 | "direct" | 268436480 | 268436480 | 402654720 | 50331648/50331648 | 0 | 0/0 |
| builder-k1-b268435200-r3 | 3 | 4 | 1 | 268435200 | 8388608 | "direct" | 1073740800 | 1073740800 | 1610611200 | 50331648/50331648 | 0 | 0/0 |
| builder-k2-b67109120-r3 | 3 | 4 | 2 | 67109120 | 8388608 | "direct" | 536872960 | 536872960 | 805309440 | 50331648/50331648 | 0 | 0/0 |
| builder-force-host | 3 | 4 | 2 | 67109120 | 8388608 | "host" | 536872960 | 536872960 | 805309440 | 8388608/8388608 | 8388608 | 536872960/805309440 |

| Workload | Control median ms | Candidate median ms | Control p90 ms | Candidate p90 ms | C/B median paired ratio | C/B p90 ratio | B/B median ratio | B/B p90 ratio |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| builder-k1-b1048832-r3 | 0.321534 | 0.310484 | 0.323006 | 0.310812 | 0.9669 | 0.9722 | 0.9911 | 0.9969 |
| builder-k1-b67109120-r0 | 9.095187 | 13.723227 | 9.199925 | 13.751204 | 1.5110 | 1.5124 | 1.0308 | 1.0374 |
| builder-k1-b67109120-r1 | 9.078633 | 13.706254 | 9.101113 | 13.716326 | 1.5098 | 1.5109 | 1.0031 | 1.0040 |
| builder-k1-b67109120-r2 | 9.272523 | 13.732513 | 9.279267 | 13.877571 | 1.4814 | 1.4956 | 0.9974 | 0.9992 |
| builder-k1-b67109120-r3 | 9.257322 | 13.692111 | 9.260586 | 13.847776 | 1.4804 | 1.4961 | 1.0034 | 1.0039 |
| builder-k1-b268435200-r3 | 33.991980 | 52.665763 | 34.019035 | 52.677410 | 1.5493 | 1.5547 | 1.0008 | 1.0015 |
| builder-k2-b67109120-r3 | 17.559761 | 26.713798 | 17.570202 | 26.721758 | 1.5213 | 1.5224 | 0.9992 | 1.0006 |
| builder-force-host | 52.781887 | 52.784075 | 52.805056 | 52.833492 | 1.0003 | 1.0006 | 0.9997 | 1.0001 |

All raw measured host intervals, in ns (warmups remain in the archive):

| Workload | Comparison | Baseline ns | Other arm ns |
| --- | --- | --- | --- |
| builder-k1-b1048832-r3 | baseline_candidate | 323374, 318915, 321534 | 309434, 310484, 310894 |
| builder-k1-b1048832-r3 | baseline_baseline | 319864, 322174, 311334 | 317014, 311014, 310804 |
| builder-k1-b67109120-r0 | baseline_candidate | 9062597, 9095187, 9226109 | 13693667, 13758198, 13723227 |
| builder-k1-b67109120-r0 | baseline_baseline | 9084307, 9065837, 9261529 | 9364472, 9419602, 9107937 |
| builder-k1-b67109120-r1 | baseline_candidate | 9078633, 9078172, 9106733 | 13718844, 13706254, 13704744 |
| builder-k1-b67109120-r1 | baseline_baseline | 9075183, 9085733, 9092183 | 9103142, 9085542, 9130083 |
| builder-k1-b67109120-r2 | baseline_candidate | 9272523, 9240963, 9280953 | 13732513, 13690024, 13913835 |
| builder-k1-b67109120-r2 | baseline_baseline | 9294343, 9293153, 9294474 | 9290953, 9237274, 9270433 |
| builder-k1-b67109120-r3 | baseline_candidate | 9261402, 9257322, 9249203 | 13680831, 13886692, 13692111 |
| builder-k1-b67109120-r3 | baseline_baseline | 9245772, 9282253, 9251912 | 9276972, 9285302, 9289373 |
| builder-k1-b268435200-r3 | baseline_candidate | 33854467, 34025799, 33991980 | 52680322, 52665763, 52663153 |
| builder-k1-b268435200-r3 | baseline_baseline | 33879478, 33846198, 34886810 | 33937588, 33874258, 33930969 |
| builder-k2-b67109120-r3 | baseline_candidate | 17559761, 17572812, 17551082 | 26713798, 26699658, 26723748 |
| builder-k2-b67109120-r3 | baseline_baseline | 17574590, 17558450, 17599201 | 17537751, 17545221, 17616152 |
| builder-force-host | baseline_candidate | 52781887, 52810848, 52768898 | 52777495, 52845846, 52784075 |
| builder-force-host | baseline_baseline | 52780053, 52840223, 52794723 | 52762831, 52814121, 52802752 |
