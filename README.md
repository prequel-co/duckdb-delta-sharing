# Duck Delta Share

DuckDB extension for Delta Sharing protocol. By loading this extension, DuckDB could now act as a Delta Sharing client by using `delta_share_list` and `delta_share_read` functions.
- Disclaimer: This extension is in experimental phase. Please do note that the extension might have bugs. If you find any, please raise an issue/discussion.

## Functions

- `delta_share_list(<0-2>)` → Can list `shares`, `schemas`, and `tables` depending on provided argument/s.
  - `delta_share_list()`                            → List all shares under Delta Sharing server
  - `delta_share_list('share_name')`                → List all schemas under `share_name`
  - `delta_share_list('share_name', 'schema_name')` → List all tables under `schema_name`

- `delta_share_read('share_name', 'schema_name', 'table_name')` → Reads all files under `table_name`.
  - Supports predicate pushdown and partition column isolation (experimental).

- `delta_share_list_files(<3-4>)` → Lists all files under specified table.  
  - `delta_share_list('share_name', 'schema_name', 'table_name')` → All files under table. No projection for `partition_columns`.
  - `delta_share_list('share_name', 'schema_name', 'table_name', '<filter_clause>')` → Filter via partition column. i.e. `date > 20251231`.

## Dependencies

- `httpfs`
- `read_parquet`

## Features & Modern Native Pipeline

- 🚀 Fully Native: Uses DuckDB `MultiFileReader` to bind directly to DuckDB's ultra-performant C++ `parquet_scan` operator.
- Automatically handles dynamic partition columns using `constant_map` without complex query injections.
- Deletion Vector framework initialized and mapped correctly for integration with external RoaringBitmap decode sources.

## Configuration

This extension uses internal config to store Delta Sharing credentials. You can set configuration by doing SET or running DuckDB with environment variables.
- `delta_sharing_endpoint`     → Base endpoint for Delta Sharing server (i.e. `localhost:8080/delta-sharing`).
- `delta_sharing_bearer_token` → Bearer token without the `Bearer ` prefix
