Query intervals and publication intervals are distinct. Publication timing, when available, is the sum of all finish-attempt preparation-through-completion intervals in a query; it is not necessarily query critical-path time. Publication comparisons require equivalent admitted and published work. SQL per-publication root/K/B/C are not exposed by the snapshot.

| Query | Metric | Control median ms | Candidate median ms | Control p90 ms | Candidate p90 ms | C/B median paired ratio | C/B p90 ratio | B/B median ratio | B/B p90 ratio | Excluded C/B pairs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | query_elapsed_ns | 58.565044 | 61.685562 | 59.481860 | 63.464620 | 1.0533 | 1.0978 | 1.0283 | 1.0805 | {} |
| sql-q4 | host_elapsed_ns | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | {"publication_work_not_equivalent_or_not_published": 3} |
| sql-q21 | query_elapsed_ns | 238.346926 | 254.443982 | 241.939674 | 255.301784 | 1.0522 | 1.0720 | 1.0211 | 1.0652 | {} |
| sql-q21 | host_elapsed_ns | 24.407922 | 35.694651 | 24.537236 | 36.347217 | 1.4672 | 1.4822 | 1.0581 | 1.0657 | {} |
| sql-q7 | query_elapsed_ns | 69.188158 | 74.117229 | 72.619183 | 74.211119 | 1.0729 | 1.0764 | 1.0467 | 1.0900 | {} |
| sql-q7 | host_elapsed_ns | 3.402383 | 5.010317 | 3.402575 | 5.229048 | 1.4725 | 1.5406 | 1.0067 | 1.0079 | {} |

Counter values per measured query execution (sets show all distinct observations):

| Query | Comparison/arm | Publication count | Published bytes | Input bytes | Ready bytes | Admitted keys | Contributing GPUs | Skip reason | Interception/profile/oracle |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sql-q4 | baseline_candidate/baseline | 0 | 0 | 0 | 0 | 0 | 0 | "no_accumulated_publication" | true/true/true |
| sql-q4 | baseline_candidate/candidate | 0 | 0 | 0 | 0 | 0 | 0 | "no_accumulated_publication" | true/true/true |
| sql-q4 | baseline_baseline/baseline | 0 | 0 | 0 | 0 | 0 | 0 | "no_accumulated_publication" | true/true/true |
| sql-q4 | baseline_baseline/candidate | 0 | 0 | 0 | 0 | 0 | 0 | "no_accumulated_publication" | true/true/true |
| sql-q21 | baseline_candidate/baseline | 3 | 174974624 | 699898496 | 699898496 | 3 | 12 | null | true/true/true |
| sql-q21 | baseline_candidate/candidate | 3 | 174974624 | 699898496 | 699898496 | 3 | 12 | null | true/true/true |
| sql-q21 | baseline_baseline/baseline | 3 | 174974624 | 699898496 | 699898496 | 3 | 12 | null | true/true/true |
| sql-q21 | baseline_baseline/candidate | 3 | 174974624 | 699898496 | 699898496 | 3 | 12 | null | true/true/true |
| sql-q7 | baseline_candidate/baseline | 1 | 23982720 | 95930880 | 95930880 | 1 | 4 | null | true/true/true |
| sql-q7 | baseline_candidate/candidate | 1 | 23982720 | 95930880 | 95930880 | 1 | 4 | null | true/true/true |
| sql-q7 | baseline_baseline/baseline | 1 | 23982720 | 95930880 | 95930880 | 1 | 4 | null | true/true/true |
| sql-q7 | baseline_baseline/candidate | 1 | 23982720 | 95930880 | 95930880 | 1 | 4 | null | true/true/true |

Raw measured query and publication intervals, in ns:

| Query | Comparison/arm | Query ns | Publication ns |
| --- | --- | --- | --- |
| sql-q4 | baseline_candidate/baseline | 59711064, 57632264, 58565044 | None, None, None |
| sql-q4 | baseline_candidate/candidate | 57900545, 63909384, 61685562 | None, None, None |
| sql-q4 | baseline_baseline/baseline | 58432118, 55654142, 59004884 | None, None, None |
| sql-q4 | baseline_baseline/candidate | 59749360, 60857980, 60674378 | None, None, None |
| sql-q21 | baseline_candidate/baseline | 236269245, 238346926, 242837861 | 24569564, 24407922, 24328170 |
| sql-q21 | baseline_candidate/candidate | 254443982, 249178441, 255516234 | 36510358, 35480658, 35694651 |
| sql-q21 | baseline_baseline/baseline | 233821462, 232587600, 245889363 | 23263510, 23394662, 24555624 |
| sql-q21 | baseline_baseline/candidate | 251656391, 237484280, 239971554 | 24835578, 24754777, 23248231 |
| sql-q7 | baseline_candidate/baseline | 68797684, 73476939, 69188158 | 3402623, 3392193, 3402383 |
| sql-q7 | baseline_candidate/candidate | 74117229, 73602844, 74234591 | 5010317, 5283731, 5004198 |
| sql-q7 | baseline_baseline/baseline | 70793001, 67616780, 68565699 | 3399923, 3434243, 3382582 |
| sql-q7 | baseline_baseline/candidate | 71753133, 74434109, 71765904 | 3422753, 3400183, 3410433 |
