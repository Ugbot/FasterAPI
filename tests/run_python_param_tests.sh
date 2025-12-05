#!/bin/bash
#
# Run Python parameter extraction integration tests
#

set -e

cd /Users/bengamble/FasterAPI

echo "="
echo "Python Parameter Extraction Tests"
echo "=================================="
echo

# Check if uvicorn is installed
if ! command -v uvicorn &> /dev/null; then
    echo "Error: uvicorn not found. Install with: pip install uvicorn"
    exit 1
fi

# Start server in background
echo "Starting test server on port 8091..."
DYLD_LIBRARY_PATH=build/lib python3.13 -m uvicorn tests.test_python_parameters:app --port 8091 --log-level warning > /tmp/param_test_server.log 2>&1 &
SERVER_PID=$!

# Give server time to start
sleep 3

# Check if server started
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "Error: Server failed to start. Check /tmp/param_test_server.log"
    cat /tmp/param_test_server.log
    exit 1
fi

echo "Server started (PID: $SERVER_PID)"
echo

# Run tests
echo "Running tests..."
if DYLD_LIBRARY_PATH=build/lib python3.13 tests/test_python_parameters.py; then
    echo
    echo "✅ All tests passed!"
    EXIT_CODE=0
else
    echo
    echo "❌ Some tests failed"
    EXIT_CODE=1
fi

# Cleanup
echo
echo "Stopping server..."
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

exit $EXIT_CODE
