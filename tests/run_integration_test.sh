#!/bin/bash
# Wrapper script to run integration tests with stub WebSocket server

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_SCRIPT="$SCRIPT_DIR/stub_websocket_server.py"
TEST_BINARY="$1"

if [ -z "$TEST_BINARY" ]; then
    echo "Usage: $0 <test_binary>"
    exit 1
fi

# Start the stub server
python3 "$SERVER_SCRIPT" &
SERVER_PID=$!

# Wait for server to be ready
for i in {1..50}; do
    if nc -z 127.0.0.1 9999 2>/dev/null; then
        break
    fi
    sleep 0.1
done

# Run the test
EXIT_CODE=0
"$TEST_BINARY" || EXIT_CODE=$?

# Stop the server
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

exit $EXIT_CODE
