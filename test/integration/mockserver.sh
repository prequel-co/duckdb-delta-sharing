# Start and stop a MockServer container for the integration tests.
#
# Usage (from a test script):
#   source test/integration/mockserver.sh
#   trap mockserver_stop EXIT
#   mockserver_start path/to/expectations.json   # sets MOCKSERVER_PORT

MOCKSERVER_IMAGE=${MOCKSERVER_IMAGE:-"mockserver/mockserver:9.0.0"}

mockserver_start() {
    local expectations
    expectations="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
    if ! command -v docker > /dev/null; then
        echo "ERROR: docker is required to run MockServer"
        return 1
    fi
    MOCKSERVER_CONTAINER=$(docker run -d --rm -p 127.0.0.1::1080 \
        -v "${expectations}:/config/expectations.json:ro" \
        -e MOCKSERVER_INITIALIZATION_JSON_PATH=/config/expectations.json \
        -e MOCKSERVER_LOG_LEVEL=WARN \
        "$MOCKSERVER_IMAGE")
    MOCKSERVER_PORT=$(docker port "$MOCKSERVER_CONTAINER" 1080/tcp | head -n 1 | sed 's/.*://')
    for _ in $(seq 150); do
        if curl -sf -o /dev/null -X PUT "http://127.0.0.1:${MOCKSERVER_PORT}/mockserver/status"; then
            return 0
        fi
        sleep 0.2
    done
    echo "ERROR: MockServer did not start"
    return 1
}

mockserver_stop() {
    if [ -n "${MOCKSERVER_CONTAINER:-}" ]; then
        docker rm -f "$MOCKSERVER_CONTAINER" > /dev/null 2>&1 || true
    fi
}
