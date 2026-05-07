#!/bin/bash
set -eo pipefail

echo "========================================================="
echo "Delta Sharing E2E Integration Tests (Databricks Server)"
echo "========================================================="

cd "$(dirname "$0")/../.."

DUCKDB_PATH="./build/release/duckdb"
EXT_PATH="./build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension"

if [ ! -f "$DUCKDB_PATH" ]; then
    echo "DuckDB executable not found. Please compile the extension first using 'make release'."
    exit 1
fi

if [ ! -f "$EXT_PATH" ]; then
    echo "Extension not found. Please compile the extension first using 'make release'."
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
TABLE="customers"

echo ""
echo "Running E2E tests against remote table: ${SHARE}.${SCHEMA}.orders (EXPECTED TO HAVE DELETION VECTORS)..."
echo "---------------------------------------------------------"

QUERY_ORDERS_DESC="
LOAD '${EXT_PATH}';
LOAD httpfs;
SET delta_sharing_endpoint='${DB_ENDPOINT}';
SET delta_sharing_bearer_token='${DB_TOKEN}';

DESCRIBE SELECT * FROM delta_share_read('${SHARE}', '${SCHEMA}', 'orders');
"

echo "Describing orders table to verify schema mapping..."
ORDERS_DESC=$($DUCKDB_PATH -unsigned -c "$QUERY_ORDERS_DESC")
echo "$ORDERS_DESC"

if echo "$ORDERS_DESC" | grep -q "col-"; then
    echo "ERROR: Found physical column names in logical schema! Column mapping failed."
    exit 1
fi

if ! echo "$ORDERS_DESC" | grep -q "order_payment_method"; then
    echo "ERROR: Missing added logical column 'order_payment_method'! Column mapping failed."
    exit 1
fi

QUERY_ORDERS="
LOAD '${EXT_PATH}';
LOAD httpfs;
SET delta_sharing_endpoint='${DB_ENDPOINT}';
SET delta_sharing_bearer_token='${DB_TOKEN}';

SELECT * FROM delta_share_read('${SHARE}', '${SCHEMA}', 'orders');
"
$DUCKDB_PATH -unsigned -c "$QUERY_ORDERS"

echo ""
echo "Testing Time Travel with delta_share_read..."
echo "---------------------------------------------------------"

QUERY_TIME_TRAVEL="
LOAD '${EXT_PATH}';
LOAD httpfs;
SET delta_sharing_endpoint='${DB_ENDPOINT}';
SET delta_sharing_bearer_token='${DB_TOKEN}';

SELECT count(*) as row_count FROM delta_share_read('${SHARE}', '${SCHEMA}', 'orders', timestamp '2026-04-08 18:57:48');
"
$DUCKDB_PATH -unsigned -c "$QUERY_TIME_TRAVEL"

echo ""
echo "Testing Empty Result Set (Time Travel to near-empty version)..."
echo "---------------------------------------------------------"

QUERY_EMPTY="
LOAD '${EXT_PATH}';
LOAD httpfs;
SET delta_sharing_endpoint='${DB_ENDPOINT}';
SET delta_sharing_bearer_token='${DB_TOKEN}';

SELECT count(*) as row_count FROM delta_share_read('${SHARE}', '${SCHEMA}', 'orders', timestamp '2026-04-08 18:57:40');
"
$DUCKDB_PATH -unsigned -c "$QUERY_EMPTY"

echo ""
echo "Testing Change Data Feed (CDF)..."
echo "---------------------------------------------------------"

QUERY_CDF="
LOAD '${EXT_PATH}';
LOAD httpfs;
SET delta_sharing_endpoint='${DB_ENDPOINT}';
SET delta_sharing_bearer_token='${DB_TOKEN}';

SELECT * FROM delta_share_change_data_feed('${SHARE}', '${SCHEMA}', 'orders', 0) LIMIT 10;
"
$DUCKDB_PATH -unsigned -c "$QUERY_CDF"

QUERY_CDF_COUNT="
LOAD '${EXT_PATH}';
LOAD httpfs;
SET delta_sharing_endpoint='${DB_ENDPOINT}';
SET delta_sharing_bearer_token='${DB_TOKEN}';

SELECT count(*) FROM delta_share_change_data_feed('${SHARE}', '${SCHEMA}', 'orders', 0);
"

CDF_COUNT=$($DUCKDB_PATH -unsigned -csv -noheader -c "$QUERY_CDF_COUNT" | tr -d '[:space:]')
echo "CDF Total Row Count: $CDF_COUNT"

if [[ -z "$CDF_COUNT" || "$CDF_COUNT" -eq 0 ]]; then
    echo "ERROR: CDF returned 0 rows! Expected > 0"
    exit 1
fi

echo "---------------------------------------------------------"
echo "Integration Test completed successfully."
