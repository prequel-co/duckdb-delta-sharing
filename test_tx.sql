LOAD './build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension';
LOAD httpfs;
CREATE SECRET (TYPE delta_sharing, PROVIDER env);
BEGIN TRANSACTION;
SELECT * FROM delta_share_read('prequel_dev_share', 'prequel_dev', 'orders') LIMIT 1;
COMMIT;
