# 🦆 DuckDB Delta Sharing

[![DuckDB Version](https://img.shields.io/badge/DuckDB-v1.5.1-blue)](https://duckdb.org/)

[![forthebadge](/badges/powered-by-black-magic.svg)](https://forthebadge.com)
[![forthebadge](https://forthebadge.com/api/badges/community/019c42fc-aa6b-7206-8a58-cbaf73bf23fd.svg)](https://forthebadge.com)
[![forthebadge](/badges/60-percent-of-the-time-works-every-time.svg)](https://forthebadge.com)


The most efficient way to query **Delta Lake** tables directly from **DuckDB**. This extension implements the [Delta Sharing protocol](https://delta.io/sharing/), allowing you to stream data from remote Delta Sharing servers with native performance.

---

## 🚀 Key Features

*   🏎️ **Ultra-Fast Native Reader**: Built directly on DuckDB's `MultiFileReader` and C++ Parquet scanner. No Python overhead.
*   🕒 **Time Travel**: Query your data as it existed at any point in history.
*   🔄 **Change Data Feed (CDF)**: Easily track incremental additions, removals, and updates.
*   📉 **Advanced Predicate Pushdown**: Filters are pushed to the server to minimize data transfer.
*   🔒 **Secure by Design**: Supports OAuth/Bearer token authentication and industry-standard encryption.
*   📊 **Query Telemetry**: Optional SQL tracking to help administrators optimize performance.

---

## 🛠️ Getting Started

### 1. Installation

Inside your DuckDB session, run:

```sql
INSTALL duck_delta_share;
LOAD duck_delta_share;
-- Required for network access
INSTALL httpfs;
LOAD httpfs;
```

### 2. Basic Configuration

Set your sharing endpoint and bearer token:

```sql
SET delta_sharing_endpoint = 'https://your-delta-sharing-server.com/api/2.0/delta-sharing';
SET delta_sharing_bearer_token = 'your_private_token';
```

---

## 📂 Functional Reference

### Discovering Content
List available shares, schemas, and tables:

```sql
-- List all shares
SELECT * FROM delta_share_list();

-- List schemas in a share
SELECT * FROM delta_share_list('my_share');

-- List tables in a schema
SELECT * FROM delta_share_list('my_share', 'my_schema');
```

### Reading Tables
Query a remote Delta table just like a local file:

```sql
SELECT * FROM delta_share_read('my_share', 'my_schema', 'my_table') 
WHERE year = 2024 
LIMIT 100;
```

---

## 📖 What is Delta Sharing?

**Delta Sharing** is an open protocol for secure data sharing from a Delta Lake to any client. Instead of copying large datasets, the server provides the client with temporary, short-lived URLs to the underlying Parquet files.

### Why use this extension?
Traditional Delta Sharing clients often rely on heavy frameworks like Spark. This extension allows **DuckDB**—the world's fastest analytical database—to pull those files directly into its memory. It handles:
- **Partition Discovery**: Automatically mapping folder structures to columns.
- **Deletion Vectors**: Handling row-level deletes without rewriting files.
- **Schema Evolution**: Safely mapping Delta types to DuckDB's native types.

---

## 🕒 Time Travel
Need to see how things looked yesterday? Provide a timestamp to look back in time:

```sql
-- Query data as of a specific point in time
SELECT count(*) 
FROM delta_share_read('my_share', 'my_schema', 'my_table', '2024-04-09 12:00:00');
```

---

## 🔄 Change Data Feed (CDF)
Track exactly what changed between versions. This is perfect for incremental synchronization or auditing.

```sql
-- Get all changes starting from version 10
SELECT * 
FROM delta_share_change_data_feed('my_share', 'my_schema', 'my_table', 10);

-- Query changes within a timestamp range
SELECT _change_type, _commit_version, *
FROM delta_share_change_data_feed('my_share', 'my_schema', 'my_table', '2024-04-01', '2024-04-10');
```
*Columns like `_change_type` (`insert`, `delete`, `update`), `_commit_version`, and `_commit_timestamp` are automatically synthesized for you.*

---

## ⚙️ Configuration Options

| Setting | Type | Description | Default |
|:---|:---|:---|:---|
| `delta_sharing_endpoint` | `VARCHAR` | Base URL of the Delta Sharing server | |
| `delta_sharing_bearer_token` | `VARCHAR` | Secret JWT token for authentication | |
| `delta_sharing_query_telemetry_disabled` | `BOOLEAN` | Whether to hide your SQL from the server | `false` |

### 📊 Query Telemetry
When enabled (`false` by default), the extension sends a Base64-encoded snippet of your SQL query in the `delta-sharing-query-sql` HTTP header. This allows server administrators to see which queries are being run and optimize data layout accordingly.

> [!WARNING]
> **Privacy Note**: If you use hard-coded literals (e.g., `WHERE email = 'user@example.com'`) instead of parameters, those literals will be included in the telemetry header. If privacy is a concern, set `delta_sharing_query_telemetry_disabled` to `true`.

---

## 🤝 Developed by Prequel
This extension is maintained by **Prequel**. For bugs or feature requests, please open an issue in the repository.
