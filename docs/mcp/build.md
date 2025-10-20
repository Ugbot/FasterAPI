# Building FasterAPI MCP

Complete guide to building FasterAPI MCP from source.

## Prerequisites

### Required
- **C++ compiler**: GCC 10+, Clang 12+, or MSVC 2019+
- **CMake**: 3.20 or later
- **Python**: 3.8 or later
- **Cython**: For Python bindings

### Optional
- **OpenSSL**: For JWT authentication (recommended)
- **liburing**: For io_uring support (Linux only)

### Install Dependencies

**macOS**:
```bash
brew install cmake openssl python@3.11
pip install cython
```

**Ubuntu/Debian**:
```bash
sudo apt-get install -y build-essential cmake libssl-dev python3-dev
pip install cython
```

**Fedora/RHEL**:
```bash
sudo dnf install -y gcc-c++ cmake openssl-devel python3-devel
pip install cython
```

## Build Steps

### 1. Clone Repository

```bash
git clone https://github.com/bengamble/FasterAPI.git
cd FasterAPI
```

### 2. Build C++ Library

```bash
# Quick build (recommended)
make build

# Or manual CMake
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DFA_BUILD_MCP=ON

cmake --build build
```

This builds:
- `fasterapi/_native/libfasterapi_mcp.{dylib,so,dll}` - C++ MCP library

### 3. Build Cython Bindings

```bash
cd fasterapi/mcp
python setup_proxy.py build_ext --inplace
cd ../..
```

This creates:
- `fasterapi/mcp/proxy_bindings.*.so` - Cython proxy bindings

### 4. Install Python Package

```bash
pip install -e .
```

### 5. Verify Installation

```bash
python -c "from fasterapi.mcp import MCPServer, MCPProxy; print('✅ MCP ready')"
```

## Build Options

### CMake Options

```bash
cmake -S . -B build \
  -DFA_BUILD_MCP=ON \              # Enable MCP (required)
  -DFA_BUILD_PG=ON \               # Enable PostgreSQL
  -DFA_BUILD_HTTP=ON \             # Enable HTTP server
  -DFA_ENABLE_HTTP2=ON \           # Enable HTTP/2
  -DFA_ENABLE_COMPRESSION=ON \     # Enable compression
  -DFA_ENABLE_MIMALLOC=ON \        # Use mimalloc allocator
  -DFA_SANITIZE=OFF \              # Enable sanitizers (debug)
  -DCMAKE_BUILD_TYPE=Release       # Build type
```

### Build Types

**Release** (default):
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```
- Full optimizations (-O3 -march=native -flto)
- No debug symbols
- Fastest performance

**Debug**:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```
- No optimizations (-O0)
- Debug symbols (-g)
- Easier debugging

**RelWithDebInfo**:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```
- Optimized with debug symbols
- Good for profiling

### Platform-Specific

**macOS (Apple Silicon)**:
```bash
cmake -S . -B build \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DFA_BUILD_MCP=ON
```

**Linux (io_uring)**:
```bash
cmake -S . -B build \
  -DFA_ENABLE_IO_URING=ON \
  -DFA_BUILD_MCP=ON
```

**Windows (MSVC)**:
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -DFA_BUILD_MCP=ON
cmake --build build --config Release
```

## Building Wheel

### Create Python Wheel

```bash
# Build wheel
python setup.py bdist_wheel

# Wheel will be in dist/
ls dist/*.whl
```

### Install Wheel

```bash
pip install dist/fasterapi-*.whl
```

## Development Workflow

### Incremental Builds

After modifying C++ code:

```bash
# Rebuild C++ only
cmake --build build

# Cython bindings don't need rebuild (they wrap C API)
```

After modifying Cython bindings:

```bash
cd fasterapi/mcp
python setup_proxy.py build_ext --inplace
```

After modifying Python code:

```bash
# No build needed, just run
python examples/mcp_proxy_example.py
```

### Clean Build

```bash
# Clean C++ build
cmake --build build --target clean

# Or remove build directory
rm -rf build

# Clean Cython builds
cd fasterapi/mcp
rm -f proxy_bindings*.so *.cpp
```

## Testing

### Run Tests

```bash
# C++ tests
cd build
ctest -V

# Python tests
pytest tests/

# Run specific test
pytest tests/test_mcp_integration.py -v
```

### Run Examples

```bash
# Basic server
python examples/mcp_server_example.py

# Proxy
python examples/mcp_proxy_example.py
```

## Troubleshooting

### OpenSSL Not Found

**Error**: `Could NOT find OpenSSL`

**Solution**:
```bash
# macOS
export OPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3

# Linux
sudo apt-get install libssl-dev

# Then rebuild
cmake --build build
```

### Cython Import Error

**Error**: `ImportError: cannot import name 'ProxyBindings'`

**Solution**:
```bash
cd fasterapi/mcp
python setup_proxy.py build_ext --inplace --force
```

### Library Not Found

**Error**: `libfasterapi_mcp.dylib not found`

**Solution**:
```bash
# Rebuild C++ library
cmake --build build

# Check it exists
ls fasterapi/_native/libfasterapi_mcp.*
```

### Symbol Not Found

**Error**: `Symbol not found: _mcp_proxy_create`

**Solution**: Rebuild everything
```bash
cmake --build build --clean-first
cd fasterapi/mcp
python setup_proxy.py build_ext --inplace --force
```

## Performance Profiling

### With perf (Linux)

```bash
# Build with debug info
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

# Profile
perf record -g python examples/mcp_proxy_example.py
perf report
```

### With Instruments (macOS)

```bash
# Build with debug info
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

# Profile in Instruments
instruments -t "Time Profiler" python examples/mcp_proxy_example.py
```

### With Valgrind

```bash
# Build with debug
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Check memory leaks
valgrind --leak-check=full python examples/mcp_proxy_example.py
```

## CI/CD

### GitHub Actions

```yaml
name: Build MCP

on: [push, pull_request]

jobs:
  build:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest]
        python: ['3.8', '3.9', '3.10', '3.11']

    steps:
    - uses: actions/checkout@v3

    - uses: actions/setup-python@v4
      with:
        python-version: ${{ matrix.python }}

    - name: Install dependencies
      run: |
        pip install cython pytest
        sudo apt-get install -y libssl-dev  # Ubuntu only

    - name: Build
      run: |
        make build
        cd fasterapi/mcp && python setup_proxy.py build_ext --inplace

    - name: Test
      run: |
        pytest tests/
```

## Production Build

### Optimized Build

```bash
# Maximum performance
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DFA_BUILD_MCP=ON \
  -DFA_ENABLE_COMPRESSION=ON \
  -DFA_ENABLE_MIMALLOC=ON \
  -DCMAKE_CXX_FLAGS="-O3 -march=native -flto"

cmake --build build

# Build Cython with optimizations
cd fasterapi/mcp
CFLAGS="-O3 -march=native" python setup_proxy.py build_ext --inplace
```

### Docker Build

```dockerfile
FROM python:3.11-slim AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake libssl-dev cython3

COPY . /app
WORKDIR /app

RUN make build && \
    cd fasterapi/mcp && \
    python setup_proxy.py build_ext --inplace

FROM python:3.11-slim

RUN apt-get update && apt-get install -y libssl3

COPY --from=builder /app /app
WORKDIR /app

RUN pip install -e .

CMD ["python", "examples/mcp_proxy_example.py"]
```

Build and run:
```bash
docker build -t fasterapi-mcp .
docker run -i fasterapi-mcp
```

## Summary

**Typical build**:
```bash
make build                                              # Build C++ library
cd fasterapi/mcp && python setup_proxy.py build_ext --inplace  # Build Cython
pip install -e .                                        # Install package
python examples/mcp_proxy_example.py                    # Test
```

**For wheel distribution**:
```bash
python setup.py bdist_wheel
pip install dist/fasterapi-*.whl
```

---

For more information, see:
- [Quick Start Guide](quickstart.md)
- [API Reference](api-reference.md)
- [Examples](../../examples/)
