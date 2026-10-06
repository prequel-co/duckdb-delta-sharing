#!/bin/bash
set -eo pipefail

echo "========================================================="
echo "HTTP Limit Integration Tests (local misbehaving server)"
echo "========================================================="

# A server that keeps paging, or never answers, must end the query with an
# error instead of hanging it. Each case runs under a watchdog, so a build
# without the limits fails here instead of hanging the script.

cd "$(dirname "$0")/../.."

DUCKDB_PATH=${DUCKDB_PATH:-"./build/release/duckdb"}
EXT_PATH=${EXT_PATH:-"./build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension"}
WATCHDOG=${WATCHDOG:-30}

if [ ! -f "$DUCKDB_PATH" ]; then
    echo "DuckDB executable not found. Please compile the extension first using 'make release'."
    exit 1
fi

if [ ! -f "$EXT_PATH" ]; then
    echo "Extension not found. Please compile the extension first using 'make release'."
    exit 1
fi

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

python3 test/integration/misbehaving_sharing_server.py > "${WORK_DIR}/port" &
SERVER_PID=$!
disown "$SERVER_PID"
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$WORK_DIR"' EXIT

for _ in $(seq 50); do
    [ -s "${WORK_DIR}/port" ] && break
    sleep 0.1
done
PORT=$(head -n 1 "${WORK_DIR}/port")
if [ -z "$PORT" ]; then
    echo "ERROR: misbehaving server did not start"
    exit 1
fi

answered() {
    curl -s --retry 5 --retry-connrefused "http://127.0.0.1:${PORT}/requests"
}

FAILED=0

# run_case <name> <mode> <expected text> <max requests> <max seconds> <sql>
run_case() {
    local name=$1 mode=$2 expected=$3 max_requests=$4 max_seconds=$5 sql=$6
    local before start output status=0 requests elapsed
    before=$(answered)
    start=$(date +%s)
    # httpfs provides the http_timeout setting
    output=$(perl -e 'alarm shift; exec @ARGV' "$WATCHDOG" \
        "$DUCKDB_PATH" -unsigned -csv -noheader -c "
LOAD httpfs;
LOAD '${EXT_PATH}';
CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT 'http://127.0.0.1:${PORT}/${mode}', BEARER_TOKEN 'test');
${sql}" 2>&1) || status=$?
    elapsed=$(( $(date +%s) - start ))
    requests=$(( $(answered) - before ))
    if [[ "$output" != *"$expected"* ]] || [ "$requests" -gt "$max_requests" ] || [ "$elapsed" -gt "$max_seconds" ]; then
        echo "FAIL: ${name} (exit ${status}, ${requests} requests, ${elapsed} s)"
        echo "$output" | tail -n 3
        FAILED=1
        return
    fi
    echo "PASS: ${name} (${requests} requests, ${elapsed} s)"
}

run_case "listing that returns the token it was sent" echo \
    "returned a next page token it had already returned" 2 5 \
    "SELECT count(*) FROM delta_share_list();"

run_case "listing that cycles between two tokens" cycle \
    "returned a next page token it had already returned" 3 5 \
    "SELECT count(*) FROM delta_share_list('s');"

run_case "listing that never stops paging" endless \
    "was still paging after 5 pages (delta_sharing_max_pages)" 5 5 \
    "SET delta_sharing_max_pages = 5; SELECT count(*) FROM delta_share_list('s', 'sc');"

run_case "three pages within a cap of 3" pages3 "items=3" 3 5 \
    "SET delta_sharing_max_pages = 3; SELECT 'items=' || count(*) FROM delta_share_list_all_tables('s');"

run_case "three pages over a cap of 2" pages3 \
    "was still paging after 2 pages (delta_sharing_max_pages)" 2 5 \
    "SET delta_sharing_max_pages = 2; SELECT count(*) FROM delta_share_list_all_tables('s');"

run_case "three pages with no cap" pages3 "items=3" 3 5 \
    "SET delta_sharing_max_pages = 0; SELECT 'items=' || count(*) FROM delta_share_list_all_tables('s');"

run_case "table query that links to itself" link \
    "returned a next page link it had already returned" 2 5 \
    "SELECT count(*) FROM delta_share_read('s', 'sc', 't');"

run_case "change feed that links to itself" link \
    "returned a next page link it had already returned" 2 5 \
    "SELECT count(*) FROM delta_share_change_data_feed('s', 'sc', 't', 0);"

run_case "server that stalls after accepting" stall \
    "http_timeout: 2 seconds" 1 6 \
    "SET http_timeout = 2; SELECT count(*) FROM delta_share_list();"

run_case "slow response that keeps arriving" trickle "shares=1" 1 8 \
    "SET http_timeout = 2; SELECT 'shares=' || count(*) FROM delta_share_list();"

if [ "$FAILED" -ne 0 ]; then
    echo "HTTP limit integration tests FAILED"
    exit 1
fi
echo "HTTP limit integration tests passed"
