# FasterAPI Build & Installation Guide

Complete guide for building, testing, and distributing FasterAPI with MCP support.

## Quick Start

```bash
# Install from source
git clone https://github.com/bengamble/FasterAPI.git
cd FasterAPI
pip install -e .[all]  # Installs with all features

# Or just MCP
pip install -e .[mcp]
```

## Prerequisites

### Required
- **Python**: 3.8 or later
- **C++ Compiler**: GCC 10+, Clang 12+, or MSVC 2019+
- **CMake**: 3.20 or later
- **Cython**: 3.0 or later (for MCP proxy)

### Optional
- **OpenSSL**: For JWT authentication (recommended)
- **PostgreSQL**: For database features
- **liburing**: For io_uring support (Linux only)

### Install Dependencies

**macOS**:
```bash
brew install cmake openssl python@3.11
pip install cython
```

**Ubuntu/Debian**:
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev python3-dev
pip install cython
```

**Fedora/RHEL**:
```bash
sudo dnf install -y gcc-c++ cmake openssl-devel python3-devel
pip install cython
```

## Development Build

### 1. Clone Repository

```bash
git clone https://github.com/bengamble/FasterAPI.git
cd FasterAPI
```

### 2. Install in Editable Mode

This builds both C++ libraries and Cython extensions:

```bash
# All features
pip install -e .[all]

# Or specific features
pip install -e .[mcp]      # MCP only
pip install -e .[pg]       # PostgreSQL only
pip install -e .[dev]      # Development tools
```

This will:
1. Run CMake to build C++ libraries (libfasterapi_mcp.so, libfasterapi_pg.so)
2. Build Cython extensions (proxy_bindings.so)
3. Install Python package in editable mode

### 3. Verify Installation

```bash
python -c "from fasterapi.mcp import MCPServer, MCPProxy; print('✅ FasterAPI ready')"
```

## Production Build

### Build Wheel

```bash
# Build wheel for distribution
python setup.py bdist_wheel

# Wheel will be in dist/
ls dist/fasterapi-*.whl
```

### Install Wheel

```bash
pip install dist/fasterapi-*.whl
```

### Build for Specific Platform

```bash
# macOS ARM64
python setup.py bdist_wheel --plat-name macosx_11_0_arm64

# Linux x86_64
python setup.py bdist_wheel --plat-name manylinux2014_x86_64

# Windows AMD64
python setup.py bdist_wheel --plat-name win_amd64
```

## Manual Build (Advanced)

If you need more control over the build:

### 1. Build C++ Libraries

```bash
# Configure
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DFA_BUILD_MCP=ON \
  -DFA_BUILD_PG=ON \
  -DFA_BUILD_HTTP=ON

# Build
cmake --build build -j

# Libraries will be in build/
ls build/libfasterapi_*.{so,dylib,dll}
```

### 2. Copy Libraries

```bash
mkdir -p fasterapi/_native
cp build/libfasterapi_mcp.* fasterapi/_native/
cp build/libfasterapi_pg.* fasterapi/_native/
```

### 3. Build Cython Extensions

```bash
cd fasterapi/mcp
python setup_proxy.py build_ext --inplace
cd ../..
```

### 4. Install Package

```bash
pip install -e .
```

## Testing

### Run All Tests

```bash
# Install test dependencies
pip install -e .[dev]

# Run tests
pytest tests/ -v

# With coverage
pytest tests/ -v --cov=fasterapi --cov-report=html
```

### Run C++ Tests

```bash
cd build
ctest -V
```

### Run Specific Test

```bash
pytest tests/test_mcp_integration.py::TestMCPProxy -v
```

### Run Examples

```bash
# Basic MCP server
python examples/mcp_server_example.py

# MCP proxy
python examples/mcp_proxy_example.py
```

## Docker Build

### Dockerfile

```dockerfile
FROM python:3.11-slim AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Install Python dependencies
RUN pip install --no-cache-dir cython wheel

# Build wheel
RUN python setup.py bdist_wheel

# Production image
FROM python:3.11-slim

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

# Copy wheel from builder
COPY --from=builder /app/dist/*.whl /tmp/

# Install wheel
RUN pip install --no-cache-dir /tmp/*.whl && rm /tmp/*.whl

WORKDIR /app

# Run proxy by default
CMD ["python", "-m", "fasterapi.mcp", "proxy"]
```

### Build and Run

```bash
docker build -t fasterapi:latest .
docker run -i fasterapi:latest python examples/mcp_proxy_example.py
```

## CI/CD

### GitHub Actions

```yaml
name: Build and Test

on: [push, pull_request]

jobs:
  build:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
        python-version: ['3.8', '3.9', '3.10', '3.11', '3.12']

    steps:
    - uses: actions/checkout@v4

    - name: Set up Python
      uses: actions/setup-python@v4
      with:
        python-version: ${{ matrix.python-version }}

    - name: Install dependencies (Ubuntu)
      if: runner.os == 'Linux'
      run: |
        sudo apt-get update
        sudo apt-get install -y cmake libssl-dev

    - name: Install dependencies (macOS)
      if: runner.os == 'macOS'
      run: brew install cmake openssl

    - name: Install Python dependencies
      run: |
        python -m pip install --upgrade pip
        pip install cython wheel pytest

    - name: Build
      run: pip install -e .[all]

    - name: Test
      run: pytest tests/ -v

    - name: Build wheel
      run: python setup.py bdist_wheel

    - name: Upload wheel
      uses: actions/upload-artifact@v3
      with:
        name: wheels
        path: dist/*.whl
```

## Troubleshooting

### CMake Not Found

**Error**: `cmake: command not found`

**Solution**:
```bash
# macOS
brew install cmake

# Ubuntu
sudo apt-get install cmake

# Or use pip
pip install cmake
```

### OpenSSL Not Found

**Error**: `Could NOT find OpenSSL`

**Solution**:
```bash
# macOS
export OPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
pip install -e .[all]

# Ubuntu
sudo apt-get install libssl-dev
```

### Cython Not Found

**Error**: `Cython not available`

**Solution**:
```bash
pip install cython>=3.0
```

### Library Not Found at Runtime

**Error**: `libfasterapi_mcp.so: cannot open shared object file`

**Solution**:
```bash
# Check library exists
ls fasterapi/_native/

# If missing, rebuild
pip install -e .[all] --force-reinstall --no-cache-dir
```

### Build Fails on Windows

**Error**: Various MSVC errors

**Solution**:
```powershell
# Install Visual Studio Build Tools
# Then set environment
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

# Build
pip install -e .[all]
```

## Performance Optimization

### Release Build

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-O3 -march=native -flto" \
  -DFA_BUILD_MCP=ON

cmake --build build
```

### With Custom Allocator

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DFA_ENABLE_MIMALLOC=ON \
  -DFA_BUILD_MCP=ON

cmake --build build
```

### Profile-Guided Optimization

```bash
# 1. Build with instrumentation
cmake -S . -B build -DCMAKE_CXX_FLAGS="-fprofile-generate"
cmake --build build

# 2. Run workload
python examples/mcp_proxy_example.py < test_workload.json

# 3. Rebuild with profile data
cmake --build build --clean-first -DCMAKE_CXX_FLAGS="-fprofile-use"
```

## Distribution

### PyPI Publishing

```bash
# Build wheel
python setup.py bdist_wheel sdist

# Upload to PyPI
pip install twine
twine upload dist/*
```

### Conda Package

```yaml
# meta.yaml
package:
  name: fasterapi
  version: "0.2.0"

source:
  path: .

requirements:
  build:
    - python
    - setuptools
    - cython >=3.0
    - cmake >=3.20
    - {{ compiler('cxx') }}

  host:
    - python
    - pydantic >=2.0

  run:
    - python
    - pydantic >=2.0

test:
  imports:
    - fasterapi.mcp

  commands:
    - pytest tests/
```

Build:
```bash
conda build .
```

## Summary

**Quick install**:
```bash
pip install -e .[all]
```

**Build wheel**:
```bash
python setup.py bdist_wheel
```

**Test**:
```bash
pytest tests/ -v
```

**Run example**:
```bash
python examples/mcp_proxy_example.py
```

For more details, see:
- [MCP Documentation](docs/mcp/README.md)
- [Examples](examples/)
- [Contributing Guidelines](CONTRIBUTING.md)
