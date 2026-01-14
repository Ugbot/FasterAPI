# Autobahn WebSocket Testsuite

This directory contains the infrastructure for running the [Autobahn WebSocket Testsuite](https://github.com/crossbario/autobahn-testsuite) against FasterAPI's WebSocket implementation.

## Quick Start

```bash
# Install the testsuite
pip install autobahntestsuite

# Run tests (builds server, starts it, runs tests)
python tests/autobahn/run_autobahn_tests.py

# Quick mode (subset of tests, faster)
python tests/autobahn/run_autobahn_tests.py --quick

# Specific test cases
python tests/autobahn/run_autobahn_tests.py --cases "1.*,2.*,5.*"
```

## Manual Testing

```bash
# Build the echo server
cmake --build build --target websocket_echo_server

# Start the server
DYLD_LIBRARY_PATH=build/lib ./build/tests/websocket_echo_server

# In another terminal, run the testsuite
cd tests/autobahn
wstest -m fuzzingclient -s fuzzingclient.json

# View results
open reports/index.html
```

## Test Cases

The Autobahn testsuite covers the full RFC 6455 specification:

| Case  | Description |
|-------|-------------|
| 1.*   | Text message handling |
| 2.*   | Binary message handling |
| 3.*   | Fragmentation |
| 4.*   | Reserved bits |
| 5.*   | Ping/Pong |
| 6.*   | UTF-8 handling |
| 7.*   | Close handling |
| 9.*   | Limits and performance |
| 10.*  | Auto-fragmentation |
| 12.*  | WebSocket compression |
| 13.*  | Compression parameters |

## Files

- `websocket_echo_server.cpp` - C++ echo server for testing
- `run_autobahn_tests.py` - Test runner script
- `fuzzingclient.json` - Autobahn client config (tests our server)
- `fuzzingserver.json` - Autobahn server config (tests our client)
- `reports/` - Test output (gitignored)
