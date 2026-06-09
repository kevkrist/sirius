SELECT o_orderkey, o_comment, o_clerk
FROM orders
WHERE o_orderdate = DATE '1995-06-17'
ORDER BY o_orderkey;
