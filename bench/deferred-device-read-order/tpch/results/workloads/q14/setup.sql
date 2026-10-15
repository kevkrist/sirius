CREATE VIEW customer AS SELECT * FROM read_parquet('/data/tpch_parquet_sf100/customer/*.parquet');
CREATE VIEW lineitem AS SELECT * FROM read_parquet('/data/tpch_parquet_sf100/lineitem/*.parquet');
CREATE VIEW nation AS SELECT * FROM read_parquet('/data/tpch_parquet_sf100/nation/*.parquet');
CREATE VIEW orders AS SELECT * FROM read_parquet('/data/tpch_parquet_sf100/orders/*.parquet');
CREATE VIEW part AS SELECT * FROM read_parquet('/data/tpch_parquet_sf100/part/*.parquet');
CREATE VIEW partsupp AS SELECT * FROM read_parquet('/data/tpch_parquet_sf100/partsupp/*.parquet');
CREATE VIEW region AS SELECT * FROM read_parquet('/data/tpch_parquet_sf100/region/*.parquet');
CREATE VIEW supplier AS SELECT * FROM read_parquet('/data/tpch_parquet_sf100/supplier/*.parquet');
