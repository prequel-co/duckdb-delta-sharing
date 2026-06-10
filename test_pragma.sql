LOAD './build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension';
LOAD httpfs;
CREATE SECRET (TYPE delta_sharing, PROVIDER env);
CREATE VIEW my_orders_view AS SELECT * FROM delta_share_read('prequel_dev_share', 'prequel_dev', 'orders');
PRAGMA table_info('my_orders_view');
