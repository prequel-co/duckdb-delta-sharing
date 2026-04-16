
LOAD './build/release/extension/duck_delta_share/duck_delta_share.duckdb_extension';
LOAD httpfs;
SET delta_sharing_endpoint='https://eastus-c3.azuredatabricks.net/api/2.0/delta-sharing/metastores/88b565d0-d549-486d-8854-fad58d9a179c';
SET delta_sharing_bearer_token='';

SELECT count(*) as row_count FROM delta_share_read('prequel_dev_share', 'prequel_dev', 'orders', timestamp '2026-04-08 18:57:40');

