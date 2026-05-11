LOAD './build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension';
LOAD httpfs;
CREATE SECRET test_secret (TYPE delta_sharing, PROVIDER config, ENDPOINT 'mock', BEARER_TOKEN 'mock');
SELECT * FROM delta_share_list();
