SELECT l_orderkey, l_linenumber, l_comment
FROM lineitem
WHERE l_shipdate = DATE '1850-01-01'
ORDER BY l_orderkey, l_linenumber;
