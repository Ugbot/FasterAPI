# h3i HTTP/3 Testing for FasterAPI

This directory contains the h3i integration for testing FasterAPI's HTTP/3 implementation against Cloudflare's h3i testing tool.

## What is h3i?

[h3i](https://github.com/cloudflare/quiche/tree/master/h3i) is an HTTP/3 debugging and testing tool from Cloudflare's quiche project. It provides:

- **Frame-level control**: Craft specific HTTP/3 frame sequences
- **RFC violation testing**: Send malformed traffic to test error handling
- **qlog integration**: Record and replay test scenarios
- **Interactive and scripted modes**: Both CLI and programmatic usage

## Installation

### Quick Install

```bash
./install.sh
```

### Manual Install

Requires Rust/Cargo:

```bash
# Install Rust if not present
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# Install h3i
cargo install h3i

# Verify
h3i --version
```

## Running Tests

### Full Test Suite

```bash
python3 run_h3i_tests.py
```

### Quick Tests Only

```bash
python3 run_h3i_tests.py --quick
```

### Robustness Tests Only

```bash
python3 run_h3i_tests.py --robustness
```

### Options

```
--quick         Run only basic compliance tests
--robustness    Run only robustness/malformed traffic tests
--all           Run all tests (default)
--port PORT     Server port (default: 8443)
--host HOST     Server host (default: localhost)
--timeout SECS  Test timeout per scenario (default: 30)
--report FORMAT Output format: text, json (default: text)
--no-build      Skip building the server
--server PATH   Path to HTTP/3 server binary
```

## Test Categories

### Compliance Tests

Basic RFC 9114 compliance:
- `basic_get_request` - Simple GET request
- `get_with_headers` - GET with custom headers
- `post_with_body` - POST with body data
- `settings_exchange` - SETTINGS frame negotiation
- `multiple_streams` - Concurrent requests

### Robustness Tests

Malformed/edge-case traffic handling:
- `unknown_frame_type` - Unknown frame types (should ignore)
- `grease_frame` - GREASE frames
- `empty_headers` - Minimal headers
- `large_header_value` - Large header values
- `goaway_handling` - GOAWAY handling

## Using h3i Interactively

For debugging or manual testing:

```bash
# Start the HTTP/3 server
DYLD_LIBRARY_PATH=build/lib ./build/examples/http3_server

# In another terminal, connect with h3i
h3i localhost:8443

# Interactive commands:
> headers :method GET :path / :scheme https :authority localhost:8443
> commit
> quit
```

## Recording Test Scenarios

h3i can record sessions as qlog files for replay:

```bash
# Record a session
QLOGDIR=./scenarios h3i localhost:8443

# Replay against server
h3i localhost:8443 --qlog-input scenarios/recorded.sqlog
```

## CI Integration

The test runner is designed for CI:

```yaml
# .github/workflows/ci.yml
h3i-tests:
  runs-on: ubuntu-24.04
  steps:
    - uses: actions/checkout@v4
    - uses: actions-rust-lang/setup-rust-toolchain@v1
    - name: Install h3i
      run: cargo install h3i
    - name: Run tests
      run: python3 tests/h3i/run_h3i_tests.py --report json
    - uses: actions/upload-artifact@v4
      with:
        name: h3i-results
        path: tests/h3i/reports/h3i-results.json
```

## Directory Structure

```
tests/h3i/
├── install.sh          # h3i installation script
├── run_h3i_tests.py    # Python test runner
├── README.md           # This file
├── certs/              # Auto-generated TLS certificates
├── reports/            # Test result JSON files
└── scenarios/          # qlog test scenarios (optional)
```

## Troubleshooting

### h3i not found

```bash
# Check if cargo bin is in PATH
echo $PATH | grep -q ".cargo/bin" || export PATH="$HOME/.cargo/bin:$PATH"

# Reinstall
cargo install h3i --force
```

### Server won't start

```bash
# Check if port is in use
lsof -i :8443

# Note: The default HTTP/3 server is hardcoded to port 8443.
# If you need a different port, modify the server source or use a custom binary.
```

### TLS errors

The HTTP/3 server generates its own self-signed certificate. If you see TLS errors:

```bash
# h3i should accept self-signed certs by default
# If not, restart the server to regenerate the certificate
pkill http3_server
python3 run_h3i_tests.py
```

## References

- [h3i GitHub](https://github.com/cloudflare/quiche/tree/master/h3i)
- [h3i Blog Post](https://blog.cloudflare.com/h3i/)
- [RFC 9114 - HTTP/3](https://www.rfc-editor.org/rfc/rfc9114.html)
- [RFC 9000 - QUIC](https://www.rfc-editor.org/rfc/rfc9000.html)
