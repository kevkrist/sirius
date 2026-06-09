SELECT l_orderkey, l_linenumber, l_comment
FROM lineitem
WHERE l_shipdate = DATE '1995-06-17' AND l_comment LIKE '%final%'
ORDER BY l_orderkey, l_linenumber;
