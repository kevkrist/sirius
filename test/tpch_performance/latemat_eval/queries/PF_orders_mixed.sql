SELECT o_orderkey, o_orderdate, o_orderpriority, o_comment
FROM orders
WHERE o_orderdate BETWEEN DATE '1996-01-01' AND DATE '1996-01-03'
ORDER BY o_orderkey;
