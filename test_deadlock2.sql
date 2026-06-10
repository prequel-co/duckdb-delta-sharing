LOAD './build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension';
LOAD httpfs;
CREATE SECRET (TYPE delta_sharing, PROVIDER env);
SELECT * FROM information_schema.columns;
