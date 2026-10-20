#!/bin/bash
set -eo pipefail

echo "========================================================="
echo "Empty Table Integration Tests (MockServer)"
echo "========================================================="

# Reading a table with no files under a multi-child operator (UNION ALL, JOIN)
# used to crash intermittently (issue #41), so each query runs RUNS times.
# Queries whose plans DuckDB copies (window self-join, CTE inlining) used to
# return no rows for a table with files; those cases fail on every run.

cd "$(dirname "$0")/../.."

DUCKDB_PATH="./build/release/duckdb"
EXT_PATH="./build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension"
RUNS=${RUNS:-20}

if [ ! -f "$DUCKDB_PATH" ]; then
    echo "DuckDB executable not found. Please compile the extension first using 'make release'."
    exit 1
fi

if [ ! -f "$EXT_PATH" ]; then
    echo "Extension not found. Please compile the extension first using 'make release'."
    exit 1
fi

# MockServer serves canned Delta Sharing responses: table "empty" has no files,
# table "data" has one, read from test/data by path relative to the repo root.
source test/integration/mockserver.sh
trap mockserver_stop EXIT
mockserver_start test/integration/empty_table_expectations.json
PORT=$MOCKSERVER_PORT

SETUP="
LOAD '${EXT_PATH}';
CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT 'http://127.0.0.1:${PORT}', BEARER_TOKEN 'test');
"

FAILED=0

# run_query <name> <expected output> <sql>
run_query() {
    # CREATE SECRET in SETUP prints "true" before the query's own rows
    local name=$1 expected="true"$'\n'"$2" sql=$3
    local run output status
    for run in $(seq "$RUNS"); do
        status=0
        output=$("$DUCKDB_PATH" -unsigned -csv -noheader -c "${SETUP}${sql}" 2>&1) || status=$?
        if [ "$status" -ne 0 ] || [ "$output" != "$expected" ]; then
            echo "FAIL: ${name} (run ${run}/${RUNS}, exit ${status})"
            echo "$output"
            FAILED=1
            return
        fi
    done
    echo "PASS: ${name} (${RUNS} runs)"
}

run_query "empty table read twice" $'0\n0' \
    "SELECT count(*) FROM delta_share_read('s', 'sc', 'empty')
     UNION ALL SELECT count(*) FROM delta_share_read('s', 'sc', 'empty');"

run_query "empty table and table with files" $'0\n3' \
    "SELECT count(*) FROM delta_share_read('s', 'sc', 'empty')
     UNION ALL SELECT count(*) FROM delta_share_read('s', 'sc', 'data');"

run_query "empty table joined with a local table" "0" \
    "SELECT count(*) FROM delta_share_read('s', 'sc', 'empty') e JOIN range(3) r ON e.id = r.range;"

run_query "empty change data feed read twice" $'0\n0' \
    "SELECT count(*) FROM delta_share_change_data_feed('s', 'sc', 'empty')
     UNION ALL SELECT count(*) FROM delta_share_change_data_feed('s', 'sc', 'empty');"

run_query "table with files read twice" $'3\n3' \
    "SELECT count(*) FROM delta_share_read('s', 'sc', 'data')
     UNION ALL SELECT count(*) FROM delta_share_read('s', 'sc', 'data');"

run_query "window partition over a table with files" "3" \
    "SELECT count(*) FROM (SELECT id, count(*) OVER (PARTITION BY id) FROM delta_share_read('s', 'sc', 'data'));"

run_query "CTE over a table with files, referenced twice" $'3\n3' \
    "WITH t AS NOT MATERIALIZED (SELECT * FROM delta_share_read('s', 'sc', 'data'))
     SELECT count(*) FROM t UNION ALL SELECT count(*) FROM t;"

if [ "$FAILED" -ne 0 ]; then
    echo "Empty table integration tests FAILED"
    exit 1
fi
echo "Empty table integration tests passed"
