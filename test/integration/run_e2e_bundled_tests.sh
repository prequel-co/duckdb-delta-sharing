#!/bin/bash
set -eo pipefail

echo "========================================================="
echo "Delta Sharing E2E Integration Tests (Bundled Binary)"
echo "========================================================="

cd "$(dirname "$0")/../.."

DUCKDB_PATH="./build/release/duckdb"

if [ ! -f "$DUCKDB_PATH" ]; then
    echo "DuckDB executable not found at $DUCKDB_PATH. Please compile the bundled binary first."
    exit 1
fi

if [ -f .env ]; then
    source .env
fi

DB_ENDPOINT=${DELTA_SHARING_ENDPOINT:-"https://eastus-c3.azuredatabricks.net/api/2.0/delta-sharing/metastores/88b565d0-d549-486d-8854-fad58d9a179c"}
DB_TOKEN=${DELTA_SHARING_BEARER_TOKEN:-""}

if [ -z "$DB_TOKEN" ]; then
    echo "DELTA_SHARING_BEARER_TOKEN not found in environment or .env file"
    exit 1
fi
SHARE="prequel_dev_share"
SCHEMA="prequel_dev"

echo "Running E2E tests against remote table using BUNDLED EXTENSIONS..."
echo "No LOAD commands should be necessary."
echo "---------------------------------------------------------"

# Note: No LOAD commands used here. Extensions should be pre-registered.
QUERY_ORDERS="
CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '${DB_ENDPOINT}', BEARER_TOKEN '${DB_TOKEN}');

SELECT * FROM delta_share_read('${SHARE}', '${SCHEMA}', 'orders') LIMIT 5;
"
$DUCKDB_PATH -unsigned -c "$QUERY_ORDERS"

echo ""
echo "Running E2E tests against remote table events using BUNDLED EXTENSIONS..."
echo "---------------------------------------------------------"

QUERY_EVENTS="
CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '${DB_ENDPOINT}', BEARER_TOKEN '${DB_TOKEN}');

SELECT * FROM delta_share_read('${SHARE}', '${SCHEMA}', 'events') LIMIT 5;
"
$DUCKDB_PATH -unsigned -c "$QUERY_EVENTS"

echo ""
echo "Running E2E tests against remote table master_import_table using BUNDLED EXTENSIONS..."
echo "---------------------------------------------------------"

QUERY_MASTER_TABLE="
CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '${DB_ENDPOINT}', BEARER_TOKEN '${DB_TOKEN}');

SELECT * FROM delta_share_read('${SHARE}', '${SCHEMA}', 'master_import_table') LIMIT 5;
"
$DUCKDB_PATH -unsigned -c "$QUERY_MASTER_TABLE"

echo ""
echo "Testing Time Travel with delta_share_read (Bundled)..."
echo "---------------------------------------------------------"

QUERY_TIME_TRAVEL="
CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '${DB_ENDPOINT}', BEARER_TOKEN '${DB_TOKEN}');

SELECT count(*) as row_count FROM delta_share_read('${SHARE}', '${SCHEMA}', 'orders', timestamp '2026-04-08 18:57:48');
"
$DUCKDB_PATH -unsigned -c "$QUERY_TIME_TRAVEL"

echo ""
echo "Testing Change Data Feed (CDF) (Bundled)..."
echo "---------------------------------------------------------"

QUERY_CDF="
CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '${DB_ENDPOINT}', BEARER_TOKEN '${DB_TOKEN}');

SELECT * FROM delta_share_change_data_feed('${SHARE}', '${SCHEMA}', 'orders', 0) LIMIT 10;
"
$DUCKDB_PATH -unsigned -c "$QUERY_CDF"

echo "---------------------------------------------------------"
echo "Bundled Integration Test completed successfully."
