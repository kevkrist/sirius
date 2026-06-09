SELECT c_custkey, c_name, c_address, c_comment
FROM customer
WHERE c_acctbal > 9900.00
ORDER BY c_custkey;
