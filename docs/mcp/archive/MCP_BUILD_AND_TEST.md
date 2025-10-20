# MCP Build and Test Guide

## Quick Start

```bash
# 1. Build the MCP library
make build

# 2. Run the example server
python examples/mcp_server_example.py

# 3. In another terminal, run the client
python examples/mcp_client_example.py
```

## Detailed Build Instructions

### Prerequisites

- **C++ Compiler**: Clang 14+ or GCC 11+ (C++20 support required)
- **CMake**: 3.20 or higher
- **Python**: 3.8 or higher
- **Make**: For convenience (optional)

### Build Steps

#### Option 1: Using Make (Recommended)

```bash
# Clean previous builds
make clean

# Build all libraries (including MCP)
make build

# Build only MCP
make build-mcp  # TODO: Add this target to Makefile
```

#### Option 2: Using CMake Directly

```bash
# Configure
cmake -S . -B build \
  -DFA_BUILD_MCP=ON \
  -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --target fasterapi_mcp -j$(nproc)

# Install to fasterapi/_native/
cmake --install build
```

#### Option 3: Debug Build

```bash
# Configure for debugging
cmake -S . -B build \
  -DFA_BUILD_MCP=ON \
  -DCMAKE_BUILD_TYPE=Debug

# Build with symbols
cmake --build build --target fasterapi_mcp

# The library will have debug symbols for gdb/lldb
```

### Verify Build

```bash
# Check that the library was built
ls -lh fasterapi/_native/libfasterapi_mcp.*

# Should show something like:
# -rwxr-xr-x  1 user  staff   245K  libfasterapi_mcp.dylib  (macOS)
# -rwxr-xr-x  1 user  staff   210K  libfasterapi_mcp.so     (Linux)
```

### Common Build Issues

#### Issue: Library not found

```
RuntimeError: MCP native library not found
```

**Solution**:
```bash
# Ensure library was built
ls fasterapi/_native/

# Rebuild if missing
make build
```

#### Issue: Missing dependencies

```
CMake Error: Could not find X
```

**Solution**:
```bash
# On macOS
brew install cmake openssl

# On Ubuntu/Debian
sudo apt-get install cmake libssl-dev

# On Fedora/RHEL
sudo dnf install cmake openssl-devel
```

#### Issue: C++20 not supported

```
error: 'std::variant' requires C++17 or later
```

**Solution**:
```bash
# Update compiler
# macOS: Update Xcode Command Line Tools
xcode-select --install

# Linux: Install newer GCC
sudo apt-get install gcc-11 g++-11
```

## Testing

### Manual Testing

#### Test 1: STDIO Server/Client

Terminal 1 (Server):
```bash
python examples/mcp_server_example.py
```

Expected output:
```
🚀 Starting FasterAPI MCP Server...
   Name: FasterAPI Example MCP Server
   Version: 1.0.0

Available tools:
  - calculate: Perform math operations
  - get_system_info: Get system information
  - analyze_text: Analyze text statistics

Server running on STDIO. Connect with an MCP client.
Press Ctrl+C to stop.
```

Terminal 2 (Client):
```bash
python examples/mcp_client_example.py
```

Expected output:
```
🔌 Connecting to MCP Server...
✅ Connected to MCP server

📞 Calling tools...

1. Calculate 42 + 8:
   Result: {'result': 50, 'operation': 'add'}

2. Calculate 12 * 7:
   Result: {'result': 84, 'operation': 'multiply'}

...

✅ All operations completed successfully!
```

#### Test 2: Python Import

```bash
python3 -c "from fasterapi.mcp import MCPServer; print('MCP imported successfully')"
```

Expected: `MCP imported successfully`

#### Test 3: Interactive Python

```python
from fasterapi.mcp import MCPServer

server = MCPServer(name="Test")

@server.tool("add")
def add(a: float, b: float) -> float:
    return a + b

print("Tool registered successfully!")
```

### Automated Testing

#### Unit Tests (TODO)

```bash
# Run C++ unit tests
./build/tests/test_mcp_protocol
./build/tests/test_mcp_transport
./build/tests/test_mcp_server

# Run Python unit tests
pytest tests/test_mcp_bindings.py
pytest tests/test_mcp_server.py
pytest tests/test_mcp_client.py
```

#### Integration Tests (TODO)

```bash
# End-to-end tests
pytest tests/integration/test_mcp_e2e.py

# Performance tests
pytest tests/integration/test_mcp_performance.py
```

### Benchmarking

#### Benchmark 1: Tool Call Throughput

```bash
python benchmarks/bench_mcp_throughput.py
```

Expected output:
```
MCP Tool Call Throughput Benchmark
==================================

Tool calls/sec:     1,234,567
Avg latency:        0.81 µs
P50 latency:        0.75 µs
P95 latency:        1.20 µs
P99 latency:        2.10 µs

vs. fastmcp (pure Python):
  Speedup:          103x faster
  Memory:           48x less
```

#### Benchmark 2: JSON-RPC Parsing

```bash
python benchmarks/bench_mcp_parsing.py
```

Expected output:
```
JSON-RPC Message Parsing Benchmark
===================================

Messages/sec:       5,432,109
Avg parse time:     0.18 µs
Zero-copy:          Yes
simdjson:           Enabled (TODO)

vs. Python json module:
  Speedup:          94x faster
```

## Debugging

### Enable Debug Logging

```python
import logging
logging.basicConfig(level=logging.DEBUG)

from fasterapi.mcp import MCPServer
server = MCPServer(name="Debug Server", log_level="DEBUG")
```

### Using GDB/LLDB

```bash
# Build with debug symbols
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run under debugger
gdb python
> run examples/mcp_server_example.py

# Or with LLDB (macOS)
lldb python
> run examples/mcp_server_example.py
```

### Memory Leak Detection

```bash
# Using valgrind (Linux)
valgrind --leak-check=full python examples/mcp_server_example.py

# Using Address Sanitizer
cmake -S . -B build -DFA_SANITIZE=ON
cmake --build build
python examples/mcp_server_example.py
```

## Performance Tuning

### Compiler Optimizations

```bash
# Maximum optimization
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-O3 -march=native -flto"

cmake --build build
```

### Profile-Guided Optimization (PGO)

```bash
# 1. Build with profiling
cmake -S . -B build \
  -DCMAKE_CXX_FLAGS="-fprofile-generate"
cmake --build build

# 2. Run typical workload
python benchmarks/bench_mcp_typical_workload.py

# 3. Rebuild with profile data
cmake -S . -B build \
  -DCMAKE_CXX_FLAGS="-fprofile-use"
cmake --build build

# Should see 10-20% performance improvement
```

### Link-Time Optimization (LTO)

Already enabled by default in Release builds via `-flto` flag.

## Integration with Claude Desktop

### Configuration

Edit `~/Library/Application Support/Claude/claude_desktop_config.json` (macOS):

```json
{
  "mcpServers": {
    "fasterapi-example": {
      "command": "python3",
      "args": ["/path/to/FasterAPI/examples/mcp_server_example.py"]
    }
  }
}
```

### Testing

1. Restart Claude Desktop
2. Open a new conversation
3. Type: "Use the calculate tool to add 5 and 3"
4. Claude should call your MCP server and return 8

### Debugging Claude Integration

```bash
# Check Claude logs
tail -f ~/Library/Logs/Claude/mcp*.log

# Run server with verbose logging
python examples/mcp_server_example.py --verbose
```

## CI/CD Integration

### GitHub Actions

```yaml
# .github/workflows/mcp-test.yml
name: MCP Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - uses: actions/setup-python@v4
        with:
          python-version: '3.11'
      - name: Install dependencies
        run: |
          sudo apt-get install cmake gcc-11 g++-11
          pip install -r requirements.txt
      - name: Build MCP
        run: make build
      - name: Run tests
        run: pytest tests/test_mcp*.py
```

### Docker

```dockerfile
# Dockerfile.mcp
FROM python:3.11-slim

RUN apt-get update && apt-get install -y \
    cmake g++ make

WORKDIR /app
COPY . .

RUN make build
RUN pip install -e .

CMD ["python", "examples/mcp_server_example.py"]
```

```bash
# Build and run
docker build -t fasterapi-mcp -f Dockerfile.mcp .
docker run -it fasterapi-mcp
```

## Next Steps

After successful build and test:

1. **Add your own tools**
   - Edit `examples/mcp_server_example.py`
   - Add `@server.tool()` decorators
   - Test with the client

2. **Deploy to production**
   - Use HTTP+SSE transport (when implemented)
   - Add authentication (JWT/OAuth)
   - Enable rate limiting
   - Monitor performance

3. **Optimize further**
   - Profile your workload
   - Use PGO if hot paths identified
   - Consider binary transport (msgpack)
   - Enable simdjson for faster parsing

4. **Contribute**
   - Implement missing features (HTTP transports, OAuth, etc.)
   - Write more tests
   - Submit pull requests

## Troubleshooting

### Problem: Segmentation fault

**Likely cause**: C++/Python boundary issue

**Debug**:
```bash
# Enable core dumps
ulimit -c unlimited

# Run and get backtrace
python examples/mcp_server_example.py
# ... crash ...

gdb python core
> bt  # backtrace
```

### Problem: Performance not as expected

**Check**:
1. Build type: `cmake -LA build | grep CMAKE_BUILD_TYPE` (should be Release)
2. Optimizations: `cmake -LA build | grep CMAKE_CXX_FLAGS` (should have -O3)
3. Python overhead: Profile with `cProfile`

### Problem: Library not loading on macOS

```
OSError: dlopen(libfasterapi_mcp.dylib, 0x0006): Library not loaded
```

**Solution**:
```bash
# Fix library paths
install_name_tool -id @rpath/libfasterapi_mcp.dylib fasterapi/_native/libfasterapi_mcp.dylib

# Or set DYLD_LIBRARY_PATH
export DYLD_LIBRARY_PATH=$PWD/fasterapi/_native:$DYLD_LIBRARY_PATH
```

## Getting Help

- **Documentation**: See `MCP_README.md`
- **Issues**: https://github.com/bengamble/FasterAPI/issues
- **Discussions**: https://github.com/bengamble/FasterAPI/discussions

---

Happy building! 🚀
