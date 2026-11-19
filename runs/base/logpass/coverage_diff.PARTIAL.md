
### q1  (base hot window 470 ms, tip 472 ms; joins 0; wired base 0 / tip 0)
no DF keys at base or tip
- consumer base: -
- consumer tip : -
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 1->1, GPU_SCAN 3->3

### q2  (base hot window 161 ms, tip 162 ms; joins 5; wired base 5 / tip 5)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 7 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same |
| 15 | 4p bcast INNER | 5 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 5B pushed=1 - | same |
| 25 | 4p bcast INNER | 799927 | 0 | one-shot in_list 6.1M pushed=1 | hash_in_list 781K pushed=1 - | same |
| 28 | 4p bcast INNER | 1999643 | 0 | one-shot in_list 15.3M pushed=1 | hash_in_list 1.9M pushed=1 - | same |
| 42 | 4p INNER | 638799 | 0 | accum@PARTITION(4b/4G) bloom 1.2M pushed=0 | none 0B pushed=0 attempt_skip_publications_skipped_targets_drained | same |
| 42 | 4p INNER | 638799 | 1 | - | none 0B pushed=0 attempt_skip_publications_skipped_targets_drained | same |
- consumer base: cscan probes 1/1/true: 5 chunks kept 0.2000 of 810000000; DYNAMIC_FILTER(4) keep 0.200 of 25 0ms; DYNAMIC_FILTER(12) keep 0.200 of 10000000 2ms; DYNAMIC_FILTER(22) keep 0.081 of 800000000 12ms; DYNAMIC_FILTER(34) keep 1.000 of 638799 0ms; DYNAMIC_FILTER(39) keep 1.000 of 471301 0ms
- consumer tip : cscan probes 1/1/true: 1 chunks kept 0.2000 of 10000000; cscan probes 1/2/false: 4 chunks kept 0.0040 of 800000000; DYNAMIC_FILTER(4) keep 0.200 of 25 0ms; DYNAMIC_FILTER(12) keep 0.200 of 10000000 2ms; DYNAMIC_FILTER(22) keep 1.000 of 3199708 1ms; DYNAMIC_FILTER(34) keep 1.000 of 638799 0ms; DYNAMIC_FILTER(39) keep 1.000 of 471301 0ms
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 17->24, CONCAT 57->11, HASH_JOIN 18->17, DYNAMIC_FILTER 14->3, GPU_SCAN 4->12

### q3  (base hot window 734 ms, tip 656 ms; joins 2; wired base 2 / tip 2)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 8 | 4p bcast INNER | 30003569 | 0 | one-shot bloom 57.2M pushed=1 | bloom 57.2M pushed=1 - | same |
| 15 | 4p bcast INNER | 147060214 | 0 | one-shot bloom 280.5M pushed=1 | bloom 280.5M pushed=1 - | same |
- consumer base: cscan probes 1/1/true: 8 chunks kept 0.2011 of 735048385; ckpt n=14 1819138663->10341207; DYNAMIC_FILTER(5) keep 1.000 of 147834473 7ms; DYNAMIC_FILTER(12) keep 0.869 of 2246699919 62ms
- consumer tip : cscan probes 1/1/true: 24 chunks kept 0.0833 of 1927817321; DYNAMIC_FILTER(5) keep 1.000 of 147834473 6ms; DYNAMIC_FILTER(12) keep 1.000 of 34116680 13ms
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 83->80, CONCAT 297->18, HASH_JOIN 454->167, DYNAMIC_FILTER 69->19, GPU_SCAN 885->4

### q4  (base hot window 317 ms, tip 323 ms; joins 1; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 9 | 4p RIGHT_SEMI | 149850442 | 0 | accum@PARTITION(10b/4G) bloom 109.4M pushed=1 | bloom 109.4M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 16 chunks kept 0.0395 of 3897884974; DYNAMIC_FILTER(5) keep 1.000 of 237012238 11ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 16 chunks kept 0.0395 of 3897884974; DYNAMIC_FILTER(5) keep 1.000 of 237012238 11ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 80->72, CONCAT 8->7, HASH_JOIN 20->20, BUILD_FILTER 26->27, DYNAMIC_FILTER 11->11, GPU_SCAN 1->1 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q5  (base hot window 670 ms, tip 680 ms; joins 5; wired base 4 / tip 4)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 10 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 18 | 4p bcast INNER | 5 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 5B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 25 | 4p bcast INNER | 29997971 | 0 | one-shot bloom 57.2M pushed=1 | bloom 57.2M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 32 | 4p bcast INNER | 45508350 | 0 | one-shot bloom 86.8M pushed=1 | bloom 86.8M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 9 chunks kept 0.1551 of 516896337; DYNAMIC_FILTER(7) keep 0.200 of 25 0ms; DYNAMIC_FILTER(15) keep 0.200 of 150000000 2ms; DYNAMIC_FILTER(22) keep 1.000 of 45747377 2ms; DYNAMIC_FILTER(29) keep 0.086 of 2214090977 453ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 24 chunks kept 0.0561 of 2607352286; DYNAMIC_FILTER(7) keep 0.200 of 25 2ms; DYNAMIC_FILTER(15) keep 0.200 of 150000000 3ms; DYNAMIC_FILTER(22) keep 1.000 of 45747377 3ms; DYNAMIC_FILTER(29) keep 1.000 of 189710493 33ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 1->23, CONCAT 48->35, HASH_JOIN 116->123, DYNAMIC_FILTER 457->41, GPU_SCAN 274->13 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q6  (base hot window 376 ms, tip 375 ms; joins 0; wired base 0 / tip 0)
no DF keys at base or tip
- consumer base: - <!-- RECOVERED from an earlier compare.py run (call #35, first version); not the final regeneration, verify -->
- consumer tip : - <!-- RECOVERED from an earlier compare.py run (call #35, first version); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): GPU_SCAN 1->1 <!-- RECOVERED from an earlier compare.py run (call #35, first version); not the final regeneration, verify -->

### q7  (base hot window 882 ms, tip 761 ms; joins 5; wired base 5 / tip 5)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 9 | 4p bcast INNER | 2 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 2B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 21 | 4p bcast INNER | 2 | 0 | one-shot small_in_list 0B pushed=1 (join guessed) | small_in_list 2B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 28 | 4p bcast INNER | 800280 | 0 | one-shot in_list 6.1M pushed=1 | hash_in_list 782K pushed=1 - | same kind, size 6.1M vs 782K | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 35 | 4p INNER | 145884740 | 0 | accum@PARTITION(4b/4G) bloom 278.3M pushed=1 | bloom 278.3M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 38 | 4p INNER | 12000338 | 0 | accum@PARTITION(1b/4G) bloom 22.9M pushed=1 | bloom 22.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 18 chunks kept 0.0800 of 754690081; cscan probes 1/2/false: 5 chunks kept 0.0812 of 1500000000; DYNAMIC_FILTER(6) keep 1.000 of 12000338 0ms; DYNAMIC_FILTER(18) keep 1.000 of 800280 0ms; DYNAMIC_FILTER(25) keep 1.000 of 145884740 124ms; DYNAMIC_FILTER(32) keep 0.084 of 121814490 8ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 18 chunks kept 0.0800 of 754690081; cscan probes 1/2/false: 5 chunks kept 0.0812 of 1500000000; DYNAMIC_FILTER(6) keep 1.000 of 12000338 0ms; DYNAMIC_FILTER(18) keep 1.000 of 800280 0ms; DYNAMIC_FILTER(25) keep 1.000 of 145884740 61ms; DYNAMIC_FILTER(32) keep 0.084 of 121814490 8ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 127->113, CONCAT 23->21, HASH_JOIN 66->66, DYNAMIC_FILTER 133->70, GPU_SCAN 82->5 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q8  (base hot window 974 ms, tip 1004 ms; joins 7; wired base 5 / tip 5)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 19 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 27 | 4p bcast INNER | 5 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 5B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 34 | 4p bcast INNER | 30002171 | 0 | one-shot bloom 57.2M pushed=1 | bloom 57.2M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 41 | 4p bcast INNER | 91139462 | 0 | one-shot bloom 173.8M pushed=1 | bloom 173.8M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 44 | 4p bcast INNER | 1332415 | 0 | one-shot in_list 10.2M pushed=1 | hash_in_list 1.3M pushed=1 - | same kind, size 10.2M vs 1.3M | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 8 chunks kept 0.2008 of 605715531; cscan probes 1/2/false: 50 chunks kept 0.0067 of 5999989709; DYNAMIC_FILTER(16) keep 0.200 of 25 0ms; DYNAMIC_FILTER(24) keep 0.200 of 150000000 2ms; DYNAMIC_FILTER(31) keep 1.000 of 91618588 4ms; DYNAMIC_FILTER(38) keep 1.000 of 39957901 65ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 8 chunks kept 0.2008 of 605715531; cscan probes 1/2/false: 50 chunks kept 0.0067 of 5999989709; DYNAMIC_FILTER(16) keep 0.200 of 25 0ms; DYNAMIC_FILTER(24) keep 0.200 of 150000000 2ms; DYNAMIC_FILTER(31) keep 1.000 of 91618588 4ms; DYNAMIC_FILTER(38) keep 1.000 of 39957901 65ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 5->55, CONCAT 64->18, HASH_JOIN 124->124, DYNAMIC_FILTER 71->71, GPU_SCAN 6->6 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q9  (base hot window 1392 ms, tip 1407 ms; joins 5; wired base 2 / tip 2)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 19 | 4p bcast INNER | 10870459 | 0 | one-shot bloom 20.7M pushed=1 | bloom 20.7M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 29 | 4p bcast INNER | 43481836 | 0 | one-shot bloom 82.9M pushed=1 | bloom 82.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 29 | 4p bcast INNER | 43481836 | 1 | one-shot bloom 82.9M pushed=1 | bloom 82.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/2/false: 16 chunks kept 0.9886 of 1565969860; DYNAMIC_FILTER(16) keep 0.424 of 800000000 4ms; DYNAMIC_FILTER(26) keep 0.054 of 5999989709 550ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 4 chunks kept 0.0556 of 800000000; cscan probes 1/2/false: 16 chunks kept 0.9886 of 1565969860; DYNAMIC_FILTER(16) keep 1.000 of 44477640 1ms; DYNAMIC_FILTER(26) keep 0.054 of 5999989709 468ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 286->356, CONCAT 762->635, HASH_JOIN 953->905, DYNAMIC_FILTER 554->470, GPU_SCAN 140->127 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q10  (base hot window 833 ms, tip 917 ms; joins 3; wired base 2 / tip 2)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 10 | 4p INNER | 57360614 | 0 | accum@PARTITION(7b/4G) bloom 109.4M pushed=1 | bloom 109.4M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 21 | 4p INNER | 44529913 | 0 | accum@PARTITION(4b/4G) bloom 84.9M pushed=1 | bloom 84.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 23 chunks kept 0.1268 of 683433385; ckpt n=52 266507446->161175938; DYNAMIC_FILTER(7) keep 1.000 of 116507446 1ms; DYNAMIC_FILTER(18) keep 1.000 of 44668492 1ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
<!-- LOST in recovery: q10 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q10 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q11  (base hot window 185 ms, tip 191 ms; joins 4; wired base 4 / tip 4)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
<!-- LOST in recovery: q11 table row 1 of 4 (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 table row 2 of 4 (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 table row 3 of 4 (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 table row 4 of 4 (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q12  (base hot window 704 ms, tip 823 ms; joins 1; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 8 | 4p bcast INNER | 31165956 | 0 | one-shot bloom 59.4M pushed=1 | bloom 59.4M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
<!-- LOST in recovery: q12 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q12 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q12 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q13  (base hot window 1361 ms, tip 1293 ms; joins 0; wired base 0 / tip 0)
no DF keys at base or tip
<!-- LOST in recovery: q13 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q13 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q13 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q14  (base hot window 776 ms, tip 760 ms; joins 1; wired base 0 / tip 0)
no DF keys at base or tip
<!-- LOST in recovery: q14 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q14 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q14 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q15  (base hot window 527 ms, tip 525 ms; joins 2; wired base 0 / tip 0)
no DF keys at base or tip
<!-- LOST in recovery: q15 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q15 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q15 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q16  (base hot window 325 ms, tip 335 ms; joins 2; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 11 | 4p bcast INNER | 29700215 | 0 | one-shot bloom 56.6M pushed=1 | bloom 56.6M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q16 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q16 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q16 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q17  (base hot window 487 ms, tip 486 ms; joins 2; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 7 | 4p bcast INNER | 200585 | 0 | one-shot in_list 1.5M pushed=1 | hash_in_list 196K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q17 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q17 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q17 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q18  (base hot window 1082 ms, tip 1073 ms; joins 3; wired base 2 / tip 2)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 14 | 4p bcast SEMI | 63430 | 0 | one-shot in_list 991K pushed=1 | hash_in_list 62K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
| 24 | 4p bcast INNER | 63430 | 0 | one-shot in_list 991K pushed=1 (join guessed) | hash_in_list 62K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q18 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q18 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q18 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q19  (base hot window 878 ms, tip 879 ms; joins 1; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 9 | 4p bcast INNER | 480774 | 0 | one-shot in_list 3.7M pushed=1 | hash_in_list 470K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q19 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q19 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q19 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q20  (base hot window 705 ms, tip 710 ms; joins 5; wired base 5 / tip 5)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 7 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 18 | 4p bcast SEMI | 2174140 | 0 | one-shot bloom 4.1M pushed=1 | bloom 4.1M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 31 | 4p bcast SEMI | 347054 | 0 | one-shot in_list 2.7M pushed=1 | hash_in_list 339K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 31 | 4p bcast SEMI | 347054 | 1 | one-shot in_list 2.7M pushed=1 | hash_in_list 339K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 45 | 4p bcast INNER | 132209 | 0 | - | none 0B pushed=0 attempt_skip_publications_skipped_targets_drained | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 45 | 4p bcast INNER | 132209 | 1 | - | none 0B pushed=0 attempt_skip_publications_skipped_targets_drained | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 49 | 1p RIGHT_SEMI | 131751 | 0 | accum@PARTITION(1b/4G) bloom 780K pushed=1 | hash_in_list 390K pushed=2 - | **kind differs** [('bloom', 1)] vs [('in_list', 2)] | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
- base decline x1: [publish_dynamic_filters] Skipping dynamic filter push: all target scans drained. <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q20 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q20 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q20 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q21  (base hot window 1432 ms, tip 1435 ms; joins 5; wired base 4 / tip 4)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 7 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
| 18 | 4p INNER | 730806711 | 0 | accum@PARTITION(5b/4G) bloom 1.4G pushed=1 | bloom 1.4G pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
| 21 | 4p bcast INNER | 399867 | 0 | one-shot in_list 3.1M pushed=1 | hash_in_list 390K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
| 33 | 4p bcast SEMI | 69665236 | 0 | one-shot bloom 132.9M pushed=1 | bloom 132.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q21 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q21 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q21 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q22  (base hot window 363 ms, tip 339 ms; joins 2; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
<!-- LOST in recovery: q22 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q22 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q22 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->


TOTAL pushed filters: base 46 tip 46 | same 45 kind-differs 1 base-only 0 tip-only 0
