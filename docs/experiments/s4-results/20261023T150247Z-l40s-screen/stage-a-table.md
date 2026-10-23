| Workload | Pattern / ordered edges | Root | Issuer | Logical bytes / repetition | Host elapsed ns (3 repetitions) | Median logical GB/s |
| --- | --- | ---: | --- | ---: | --- | ---: |
| transport-00 | pair: 0→1 | 1 | destination_read | [67109120] | 2562353, 2560973, 2561743 | 26.197 |
| transport-01 | pair: 0→2 | 2 | destination_read | [67109120] | 2563963, 2564013, 2568652 | 26.173 |
| transport-02 | pair: 0→3 | 3 | destination_read | [67109120] | 2564423, 2565223, 2563563 | 26.169 |
| transport-03 | pair: 1→0 | 0 | destination_read | [67109120] | 2564733, 2565924, 2564994 | 26.163 |
| transport-04 | pair: 1→2 | 2 | destination_read | [67109120] | 2562483, 2566173, 2563173 | 26.182 |
| transport-05 | pair: 1→3 | 3 | destination_read | [67109120] | 2564283, 2564344, 2564133 | 26.171 |
| transport-06 | pair: 2→0 | 0 | destination_read | [67109120] | 2560403, 2562903, 2560683 | 26.208 |
| transport-07 | pair: 2→1 | 1 | destination_read | [67109120] | 2566082, 2565003, 2566063 | 26.153 |
| transport-08 | pair: 2→3 | 3 | destination_read | [67109120] | 2560003, 2561633, 2560113 | 26.213 |
| transport-09 | pair: 3→0 | 0 | destination_read | [67109120] | 2563673, 2562893, 2563952 | 26.177 |
| transport-10 | pair: 3→1 | 1 | destination_read | [67109120] | 2564973, 2563453, 2565123 | 26.164 |
| transport-11 | pair: 3→2 | 2 | destination_read | [67109120] | 2564733, 2564213, 2564392 | 26.170 |
| transport-12 | all_pairs: 0→1; 0→2; 0→3; 1→0; 1→2; 1→3; 2→0; 2→1; 2→3; 3→0; 3→1; 3→2 | 0 | destination_read | [805309440] | 21930753, 22173376, 21882213 | 36.721 |
| transport-13 | root_inbound: 1→0; 2→0; 3→0 | 0 | destination_read | [201327360] | 7638788, 7639029, 7639179 | 26.355 |
| transport-14 | root_outbound: 0→1; 0→2; 0→3 | 0 | destination_read | [201327360] | 7658049, 7659529, 7657279 | 26.290 |
| transport-15 | root_inbound: 0→1; 2→1; 3→1 | 1 | destination_read | [201327360] | 7640718, 7638599, 7638288 | 26.357 |
| transport-16 | root_outbound: 1→0; 1→2; 1→3 | 1 | destination_read | [201327360] | 7659079, 7656399, 7657708 | 26.291 |
| transport-17 | root_inbound: 0→2; 1→2; 3→2 | 2 | destination_read | [201327360] | 7642549, 7640888, 7639419 | 26.349 |
| transport-18 | root_outbound: 2→0; 2→1; 2→3 | 2 | destination_read | [201327360] | 7659119, 7658139, 7658658 | 26.288 |
| transport-19 | root_inbound: 0→3; 1→3; 2→3 | 3 | destination_read | [201327360] | 7644789, 7642199, 7643279 | 26.340 |
| transport-20 | root_outbound: 3→0; 3→1; 3→2 | 3 | destination_read | [201327360] | 7658279, 7656989, 7661819 | 26.289 |
| transport-21 | pair: 0→1 | 1 | source_write | [67109120] | 2561524, 2561943, 2560783 | 26.199 |
| transport-22 | pair: 0→2 | 2 | source_write | [67109120] | 2561003, 2560633, 2560794 | 26.206 |
| transport-23 | pair: 0→3 | 3 | source_write | [67109120] | 2568483, 2558803, 2557803 | 26.227 |
| transport-24 | pair: 1→0 | 0 | source_write | [67109120] | 2558843, 2558892, 2560113 | 26.226 |
| transport-25 | pair: 1→2 | 2 | source_write | [67109120] | 2563313, 2561643, 2562973 | 26.184 |
| transport-26 | pair: 1→3 | 3 | source_write | [67109120] | 2559953, 2562093, 2561203 | 26.202 |
| transport-27 | pair: 2→0 | 0 | source_write | [67109120] | 2563303, 2559673, 2559223 | 26.218 |
| transport-28 | pair: 2→1 | 1 | source_write | [67109120] | 2563213, 2563323, 2562513 | 26.182 |
| transport-29 | pair: 2→3 | 3 | source_write | [67109120] | 2560613, 2558813, 2558403 | 26.227 |
| transport-30 | pair: 3→0 | 0 | source_write | [67109120] | 2562423, 2562693, 2562553 | 26.188 |
| transport-31 | pair: 3→1 | 1 | source_write | [67109120] | 2558263, 2557653, 2558033 | 26.235 |
| transport-32 | pair: 3→2 | 2 | source_write | [67109120] | 2561242, 2559692, 2562643 | 26.202 |
| transport-33 | all_pairs: 0→1; 0→2; 0→3; 1→0; 1→2; 1→3; 2→0; 2→1; 2→3; 3→0; 3→1; 3→2 | 0 | source_write | [805309440] | 20859089, 20861759, 20861320 | 38.603 |
| transport-34 | root_inbound: 1→0; 2→0; 3→0 | 0 | source_write | [201327360] | 7639888, 7638799, 7639938 | 26.352 |
| transport-35 | root_outbound: 0→1; 0→2; 0→3 | 0 | source_write | [201327360] | 7647989, 7648079, 7648029 | 26.324 |
| transport-36 | root_inbound: 0→1; 2→1; 3→1 | 1 | source_write | [201327360] | 7637468, 7637299, 7638669 | 26.360 |
| transport-37 | root_outbound: 1→0; 1→2; 1→3 | 1 | source_write | [201327360] | 7649169, 7649368, 7649239 | 26.320 |
| transport-38 | root_inbound: 0→2; 1→2; 3→2 | 2 | source_write | [201327360] | 7643239, 7641089, 7642969 | 26.342 |
| transport-39 | root_outbound: 2→0; 2→1; 2→3 | 2 | source_write | [201327360] | 7650718, 7651228, 7648129 | 26.315 |
| transport-40 | root_inbound: 0→3; 1→3; 2→3 | 3 | source_write | [201327360] | 7642149, 7641259, 7642359 | 26.344 |
| transport-41 | root_outbound: 3→0; 3→1; 3→2 | 3 | source_write | [201327360] | 7650939, 7650348, 7650669 | 26.315 |
