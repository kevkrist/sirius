| q | windows | DF keys tip / arm | key set equal | kept-line classes (N of M ran, complete) equal; line counts tip / arm | kept X/Y tip | kept X/Y arm | DYNAMIC_FILTER rows in/out equal | rows PRODUCED by every operator equal (inputs of CONCAT are logged per task and vary) | filtered scan pipelines whose first task precedes a later publication (tip / arm) | min gap last publication -> first task of a filtered scan started after it, ms (tip / arm) |
|---|---|---|---|---|---|---|---|---|---|---|
| 2 | ok / ok | 6 / 6 | True | True [(('1', '1', 'true'), 1), (('1', '2', 'false'), 4)] / [(('1', '1', 'true'), 1), (('1', '2', 'false'), 4)] | 5199351/810000000 | 5199351/810000000 | True (5 ops) | True | 5 / 5 | 1 / 0 |
| 3 | ok / ok | 2 / 2 | True | True [(('1', '1', 'true'), 24)] / [(('1', '1', 'true'), 20)] | 160514096/1927817321 | 157344094/1629641168 | True (2 ops) | True | 1 / 1 | 43 / 32 |
| 5 | ok / ok | 4 / 4 | True | True [(('1', '1', 'true'), 24)] / [(('1', '1', 'true'), 20)] | 146228618/2607352286 | 128617198/2049906106 | True (4 ops) | True | 3 / 3 | 1 / 1 |
| 7 | ok / ok | 5 / 5 | True | True [(('1', '1', 'true'), 18), (('1', '2', 'false'), 5)] / [(('1', '1', 'true'), 16), (('1', '2', 'false'), 5)] | 182211392/2254690081 | 176265017/2180355995 | True (4 ops) | True | 3 / 3 | 13 / 14 |
| 8 | ok / ok | 5 / 5 | True | True [(('1', '1', 'true'), 8), (('1', '2', 'false'), 50)] / [(('1', '1', 'true'), 8), (('1', '2', 'false'), 50)] | 161578660/6605705240 | 161578660/6605705240 | True (4 ops) | True | 3 / 3 | 0 / 1 |
| 9 | ok / ok | 3 / 3 | True | True [(('1', '1', 'true'), 4), (('1', '2', 'false'), 16)] / [(('1', '1', 'true'), 4), (('1', '2', 'false'), 12)] | 1592547786/2365969860 | 1205536217/1974482266 | True (2 ops) | False diff: HASH_GROUP_BY(34) in 326116789/326116789 out 2800/2100; PARTITION(35) in 2800/2100 out 2800/2100 | 1 / 1 | 17 / 14 |
| 10 | ok / ok | 2 / 2 | True | True [(('1', '1', 'true'), 23)] / [(('1', '1', 'true'), 19)] | 86644247/683433385 | 76151670/550068161 | True (2 ops) | True | 1 / 1 | 35 / 26 |
| 11 | ok / ok | 4 / 4 | True | True [(('1', '1', 'true'), 12)] / [(('1', '1', 'true'), 12)] | 64791576/1620000000 | 64791576/1620000000 | True (4 ops) | False diff: SORT_PARTITION(47) in 234719/234355 out 234719/234355; SORT_SAMPLE(46) in 234719/234355 out 234719/234355 | 3 / 3 | 8 / 8 |
| 12 | ok / ok | 1 / 1 | True | True [(('1', '1', 'true'), 8)] / [(('1', '1', 'true'), 8)] | 30639256/1500000000 | 30639256/1500000000 | True (1 ops) | True | 0 / 0 | 146 / 18 |
| 14 | ok / ok | 0 / 0 | True | True [] / [] | 0/0 | 0/0 | True (0 ops) | True | 0 / 0 | None / None |
| 15 | ok / ok | 0 / 0 | True | True [] / [] | 0/0 | 0/0 | True (0 ops) | True | 0 / 0 | None / None |
| 16 | ok / ok | 1 / 1 | True | True [(('1', '1', 'true'), 2)] / [(('1', '1', 'true'), 2)] | 119695852/800000000 | 119695852/800000000 | True (1 ops) | True | 0 / 0 | 110 / 36 |
| 17 | ok / ok | 1 / 1 | True | True [(('1', '1', 'true'), 16)] / [(('1', '1', 'true'), 16)] | 3139534/3131945555 | 3139534/3131945555 | True (1 ops) | True | 0 / 0 | 186 / 186 |
| 18 | ok / ok | 2 / 2 | True | True [(('1', '1', 'true'), 26)] / [(('1', '1', 'true'), 23)] | 355645/5431925084 | 301157/4694685208 | True (2 ops) | True | 1 / 1 | 25 / 24 |
| 19 | ok / ok | 1 / 1 | True | True [(('1', '1', 'true'), 16)] / [(('1', '1', 'true'), 12)] | 714288/297867837 | 535761/223400647 | True (1 ops) | True | 0 / 0 | 124 / 83 |
| 20 | ok / ok | 7 / 7 | True | True [(('1', '1', 'true'), 1), (('1', '2', 'false'), 34)] / [(('1', '1', 'true'), 1), (('1', '2', 'false'), 34)] | 53480840/1720220849 | 53480840/1720220849 | True (6 ops) | True | 4 / 4 | 14 / 13 |
| 21 | ok / ok | 4 / 4 | True | True [(('1', '1', 'true'), 17), (('1', '2', 'false'), 31)] / [(('1', '1', 'true'), 13), (('1', '2', 'false'), 31)] | 423297173/9124908864 | 377538900/8346164232 | True (3 ops) | True | 2 / 2 | 15 / 15 |
| 22 | ok / ok | 1 / 1 | True | True [] / [] | 0/0 | 0/0 | True (1 ops) | True | 0 / 0 | 81 / 78 |
queries with any difference: 2
