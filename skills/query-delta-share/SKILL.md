---
name: query-delta-share
description: >
  Run SQL queries against remote Delta Sharing profiles using DuckDB.
argument-hint: <SQL or question>
allowed-tools: Bash
---

You are helping the user query data using DuckDB through the Delta Sharing protocol.
Assume the DuckDB CLI being executed inherently contains the `duckdb_delta_sharing` extension already loaded. No local file detection or local directory traversal logic is necessary.

Input: `$@`

Follow these steps in order when assisting users.

## Step 1 — Verify Environment

DuckDB requires credentials or connection strings to contact a Delta Sharing Server.
Before executing queries, ensure the appropriate credentials are known, either via OS environment variables or DuckDB session settings.

- Environment Variables: `DELTA_SHARING_ENDPOINT` and `DELTA_SHARING_BEARER_TOKEN`
- DuckDB Settings: `SET delta_sharing_endpoint = '<url>'; SET delta_sharing_bearer_token = '<token>'`

If they are missing and you encounter connection errors, politely ask the user to provide them or ensure they are loaded into your environment.

## Step 2 — Check Schema & Plan Effectively

Before you unleash complex analytical queries against remote data, build context:

1. **Discover Tables:**
   ```bash
   duckdb :memory: -csv -c "SELECT * FROM delta_share_list();"
   ```
   *(Note: 0 args lists shares, 1 arg lists schemas, 2 args lists tables. Modify accordingly.)*

2. **Understand Schema:** 
   Run `DESCRIBE` to inspect column names and types before formulating a query:
   ```bash
   duckdb :memory: -csv -c "DESCRIBE SELECT * FROM delta_share_read('<share>', '<schema>', '<table>') LIMIT 0;"
   ```

3. **Verify Performance (EXPLAIN):**
   Before running a Heavy SQL query, perform an EXPLAIN analysis. Remote tables can be massive, and you want to ensure your filters are being effectively pushed down:
   ```bash
   duckdb :memory: -csv -c "EXPLAIN SELECT ... FROM delta_share_read('<share>', '<schema>', '<table>') WHERE ...;"
   ```

## Step 3 — Generate and Execute SQL

When translating the user's intent to actual queries, strictly follow these constraints:

### Constraint A: Limits for Safety
Every generated `SELECT` query **must** append a strict `LIMIT 50` clause unless the user explicitly defines an expected upper bound. Do not query arbitrary long lists.

### Constraint B: Partition Pruning
If the DESCRIBE output shows partition columns (such as `date`, `tenant_id`, `category`), **always** incorporate a `WHERE` filter for them. This allows DuckDB to prune remote parquet paths and saves significant bandwidth.

### Constraint C: The Delta Sharing Functions
Base your query off the Delta Sharing table functions. Do not use local data paths.

- **Standard Table Read:**
  `SELECT * FROM delta_share_read('share_name', 'schema_name', 'table_name' [, timestamp]);`

- **Change Data Feed (CDF):**
  `SELECT * FROM delta_share_change_data_feed('share_name', 'schema_name', 'table_name' [, startingVersion [, endingVersion]]);`

### Constraint D: Query Execution
Execute queries using DuckDB's CLI. Since you are performing data analysis efficiently in memory, use the `:memory:` database state. Use `-json` output format for you to parse easily, and `-markdown` to return beautiful terminal results to the user.

```bash
duckdb :memory: -json <<'SQL'
<YOUR_SQL_QUERY_HERE>
SQL
```
*(Tip: Always use heredocs `<<'SQL'` to avoid shell quoting syntax issues.)*

### Constraint E: DuckDB vs PostgreSQL Discrepancies
DuckDB SQL is heavily inspired by PostgreSQL, but there are some critical differences in types, operators, and functions:
- **Integer Division:** In Postgres, `1 / 2` yields `0` (integer division). In DuckDB, the `/` operator returns a `DOUBLE` (yielding `0.5`). To perform true integer division in DuckDB, use the `//` operator.
- **Timestamps:** DuckDB uses `TIMESTAMP` (without timezone) and `TIMESTAMP_TZ` (with timezone). It strictly stores them in microsecond precision and operates using powerful extensions like `make_timestamp()` or `epoch_ms()` which replace complex Postgres interval math.
- **Date Formatting:** DuckDB does **not** support Postgres' `to_char()` or `to_date()`. You must use `strftime(date, format)` and `strptime(string, format)` instead.
- **Safe Casting:** DuckDB natively supports `TRY_CAST(expr AS type)` (which returns `NULL` on failure instead of erroring). Use this over standard `CAST` when dealing with uncertain remote data.
- **JSON & Structs:** DuckDB has a single `JSON` type; there is **no `JSONB`** as there is in Postgres. Unpack `STRUCT` fields using dot notation (`column.field.subfield`) instead of Postgres JSON extraction operators (`->>`).
- **Unique Aggregates:** DuckDB has several built-in analytical aggregates that Postgres lacks or makes difficult: `list()` (replaces `array_agg`), `arg_max(return_col, max_col)` / `arg_min(return_col, min_col)` (returns the `return_col` where `max_col` is maximized), and `histogram()`.
- **Regex:** While DuckDB supports the `~` operator, it is highly recommended to use the explicit functions `regexp_matches(string, regex)` and `regexp_extract(string, regex, group)`.

## Step 4 — Handle Large Output Payloads

Sometimes, a large output payload is unavoidable, and piping directly to the console or reading it all locally could break this chat session's token limits. If you must inspect large outputs, pipe the tool's JSON output to a file and parse it in chunks.

- **Unix / macOS:** 
  Use standard bash utilities like `jq` and `head` / `tail`.
  ```bash
  duckdb :memory: -json -c "<YOUR_QUERY>" > temp_output.json
  cat temp_output.json | jq '.[] | select(.some_col > 10)' | head -n 50
  ```

- **Windows:** 
  If you detect a Windows environment, fall back to PowerShell equivalents for JSON parsing and limits:
  ```powershell
  duckdb :memory: -json -c "<YOUR_QUERY>" > temp_output.json
  Get-Content temp_output.json | ConvertFrom-Json | Select-Object -First 50
  ```

## Step 5 — Present Results

Present the results to the user cleanly. If you truncated the output by placing limits, inform the user that only a `LIMIT` subset was shown to preserve context window safely. Summarize the analytical findings.
