# FasterAPI Benchmarks

Comprehensive benchmark suite for FasterAPI, including comparisons with other frameworks and participation in industry-standard challenges.

## 📁 Directory Structure

```
benchmarks/
├── fasterapi/          # FasterAPI-specific benchmarks
├── 1mrc/              # 1 Million Request Challenge
├── techempower/       # TechEmpower Framework Benchmarks
├── comparisons/       # Framework comparison studies
└── README.md          # This file
```

---

## 🚀 FasterAPI Benchmarks (`fasterapi/`)

Core benchmarks for FasterAPI components and comparisons with FastAPI.

### C++ Micro-Benchmarks

| Benchmark | Description | Run |
|-----------|-------------|-----|
| `bench_pure_cpp.cpp` | Pure C++ baseline (no Python) | `./build/benchmarks/bench_pure_cpp` |
| `bench_router.cpp` | Router performance test | `./build/benchmarks/bench_router` |
| `bench_hpack.cpp` | HTTP/2 HPACK compression | `./build/benchmarks/bench_hpack` |
| `bench_http1_parser.cpp` | HTTP/1.1 parser | `./build/benchmarks/bench_http1_parser` |

### Python Integration Benchmarks

| Benchmark | Description | Run |
|-----------|-------------|-----|
| `bench_fasterapi_vs_fastapi.py` | FasterAPI vs FastAPI comparison | `python3 benchmarks/fasterapi/bench_fasterapi_vs_fastapi.py` |
| `bench_complete_system.py` | Complete system integration | `python3 benchmarks/fasterapi/bench_complete_system.py` |
| `bench_futures.py` | Seastar-style futures | `python3 benchmarks/fasterapi/bench_futures.py` |
| `bench_pool.py` | PostgreSQL connection pool | `python3 benchmarks/fasterapi/bench_pool.py` |
| `bench_codecs.py` | Binary protocol codecs | `python3 benchmarks/fasterapi/bench_codecs.py` |

### Quick Start

```bash
# Build C++ benchmarks
cd build
make -j8

# Run all FasterAPI benchmarks
cd ..
./run_all_benchmarks.sh
```

### Results

See the main project documentation:
- [BENCHMARK_RESULTS.md](../BENCHMARK_RESULTS.md) - FasterAPI vs FastAPI comparison
- [PYTHON_COST_SUMMARY.md](../PYTHON_COST_SUMMARY.md) - Python overhead analysis
- [PYTHON_OVERHEAD_ANALYSIS.md](../PYTHON_OVERHEAD_ANALYSIS.md) - Deep dive

**Key Findings:**
- Pure C++ is **43x faster** than FasterAPI
- Python overhead is **98%** of request time
- But only matters for CPU-bound apps (rare!)
- For I/O-bound apps: FasterAPI overhead is **negligible** (<2%)

---

## 🏆 1 Million Request Challenge (`1mrc/`)

FasterAPI's participation in the [1 Million Request Challenge (1MRC)](https://github.com/Kavishankarks/1mrc) - processing 1,000,000 concurrent requests accurately and efficiently.

### Official Repository (Submodule)

The official 1MRC repository from [Kavishankarks/1mrc](https://github.com/Kavishankarks/1mrc) is available as a git submodule:

```bash
# Initialize/update submodule
git submodule update --init --recursive

# View official implementations
cd benchmarks/1mrc/official/
ls -la  # Go, Java Spring Boot, Rust implementations
```

### FasterAPI Implementations

Our implementations showcasing different approaches:

#### C++ Implementations

| Implementation | Description | Performance | Run |
|----------------|-------------|-------------|-----|
| **Threading** | Traditional multi-threading | ~85K req/s | `./build/benchmarks/1mrc_cpp_server` |
| **Async I/O** | kqueue/epoll/io_uring | ~120K req/s | `./build/benchmarks/1mrc_async_server` |
| **libuv** | Production-ready event loop | ~200K req/s | `./build/benchmarks/1mrc_libuv_server` |

#### Python Implementations

| Implementation | Description | Performance | Run |
|----------------|-------------|-------------|-----|
| **Standard** | Python + atomic ops | ~12K req/s | `python3 benchmarks/1mrc/python/1mrc_server.py` |
| **Async** | asyncio event loop | ~15K req/s | `python3 benchmarks/1mrc/python/1mrc_async_server.py` |
| **Native** | FasterAPI with C++ backend | ~50K req/s | `python3 benchmarks/1mrc/python/1mrc_fasterapi_native.py` |

### Running the Challenge

```bash
# Start a server (choose one)
./build/benchmarks/1mrc_libuv_server &  # C++ libuv version

# Run the test client
python3 benchmarks/1mrc/client/1mrc_client.py

# View results
cat benchmarks/1mrc/results/1MRC_RESULTS.md
```

### Results & Analysis

- [1MRC_RESULTS.md](1mrc/results/1MRC_RESULTS.md) - Main results
- [1MRC_ALL_VERSIONS_RESULTS.md](1mrc/results/1MRC_ALL_VERSIONS_RESULTS.md) - All implementations compared
- [1MRC_COMPARISON.md](1mrc/results/1MRC_COMPARISON.md) - vs Go, Java, Rust
- [1MRC_ASYNC_ANALYSIS.md](1mrc/results/1MRC_ASYNC_ANALYSIS.md) - Async I/O deep dive

**Key Achievement:**
- ✅ **200,000+ req/s** (C++ libuv) - **2.3x faster than Go**
- ✅ **100% accuracy** - All 1M requests counted correctly
- ✅ **Zero errors** - Perfect thread safety
- ✅ **Low latency** - Sub-millisecond p99

See [benchmarks/1mrc/README.md](1mrc/README.md) for details.

---

## ⚡ TechEmpower Benchmarks (`techempower/`)

Industry-standard [TechEmpower Framework Benchmarks](https://www.techempower.com/benchmarks/) implementations.

### Tests Implemented

| Test | Description | Endpoint |
|------|-------------|----------|
| **JSON** | JSON serialization | GET `/json` |
| **Plaintext** | Minimal response | GET `/plaintext` |
| **Single Query** | Database read | GET `/db` |
| **Multiple Queries** | Batch database reads | GET `/queries?queries=20` |
| **Updates** | Database updates | GET `/updates?queries=20` |
| **Fortunes** | Template rendering | GET `/fortunes` |

### Implementations

```bash
# Pure C++ (maximum performance)
./build/benchmarks/bench_techempower_cpp

# Concurrent C++ (multi-threaded)
./build/benchmarks/bench_techempower_concurrent

# Python simulation
python3 benchmarks/techempower/techempower_sim.py

# Full test suite
./benchmarks/techempower/run_techempower_tests.sh
```

### Expected Performance

Based on TechEmpower Round 22 results:

| Test | FasterAPI (est.) | Top Frameworks |
|------|------------------|----------------|
| JSON | ~500K req/s | ~600K (drogon, may-minihttp) |
| Plaintext | ~1M req/s | ~7M (may-minihttp, xitca) |
| Single Query | ~100K req/s | ~150K (drogon) |
| Fortunes | ~80K req/s | ~120K (drogon) |

---

## 📊 Framework Comparisons (`comparisons/`)

Detailed comparisons with other high-performance frameworks.

### Studies Available

- [compare_with_drogon.md](comparisons/compare_with_drogon.md) - vs Drogon (top C++ framework)
- [WHY_CPP_IS_SLOW.md](comparisons/WHY_CPP_IS_SLOW.md) - C++ performance analysis
- [WHY_SO_SLOW.md](comparisons/WHY_SO_SLOW.md) - Bottleneck identification

### Quick Comparison

```bash
# Install and benchmark against Drogon
./benchmarks/comparisons/install_and_compare_drogon.sh
```

---

## 🎯 Running All Benchmarks

### Quick Run

```bash
# From project root
./run_all_benchmarks.sh
```

### Manual Run

```bash
# 1. Build C++ components
cd build
cmake ..
make -j8

# 2. Run FasterAPI benchmarks
./benchmarks/bench_pure_cpp
./benchmarks/bench_router
./benchmarks/bench_hpack
cd ..
python3 benchmarks/fasterapi/bench_complete_system.py

# 3. Run 1MRC challenge
./build/benchmarks/1mrc_libuv_server &
SERVER_PID=$!
sleep 1
python3 benchmarks/1mrc/client/1mrc_client.py
kill $SERVER_PID

# 4. Run TechEmpower tests
./benchmarks/techempower/run_techempower_tests.sh
```

---

## 📈 Performance Summary

### FasterAPI Achievements

| Category | Metric | Value |
|----------|--------|-------|
| **C++ Hot Paths** | Router match | 29 ns |
| | HTTP/1.1 parse | 12 ns |
| | HPACK compression | 6.7 ns |
| **Request Processing** | Pure C++ | 0.15 µs |
| | With Python | 6.5 µs |
| **Throughput** | 1MRC Challenge | 200K req/s |
| | Max (single core) | 1.6M req/s |
| **vs FastAPI** | Routing | **17x faster** |
| | Parsing | **66x faster** |

### Industry Context

**1MRC Challenge Results:**
- FasterAPI C++ libuv: **200K req/s** 🥇
- Go (official): 85K req/s
- Java Spring Boot: 15K req/s
- Rust: 120K req/s
- Python (standard): 12K req/s

**TechEmpower Context:**
- Top frameworks: 500K-7M req/s (depending on test)
- FasterAPI target: Top 10 in C++ category
- Python frameworks: Typically 10K-50K req/s

---

## 🔧 Build Configuration

All C++ benchmarks use optimized build settings:

```cmake
CMAKE_BUILD_TYPE=Release
CMAKE_CXX_FLAGS="-O3 -mcpu=native -flto -fno-exceptions -fno-rtti"
```

Compiler: clang (Apple LLVM) or gcc 11+

---

## 📖 Documentation

### Main Documentation
- [../BENCHMARK_RESULTS.md](../BENCHMARK_RESULTS.md) - Complete benchmark results
- [../PYTHON_COST_SUMMARY.md](../PYTHON_COST_SUMMARY.md) - Python overhead summary
- [../FINAL_BENCHMARKS.md](../FINAL_BENCHMARKS.md) - Final benchmark report

### Specialized Docs
- [fasterapi/README.md](fasterapi/README.md) - FasterAPI benchmarks (if created)
- [1mrc/README.md](1mrc/README.md) - 1MRC challenge details
- [techempower/README.md](techempower/README.md) - TechEmpower tests (if created)

---

## 🤝 Contributing

Want to add more benchmarks?

1. **Choose category**: fasterapi, 1mrc, techempower, or comparisons
2. **Follow conventions**: See existing benchmarks in each directory
3. **Document results**: Add to appropriate results file
4. **Update this README**: Add to the table above

---

## 📊 Quick Reference

**Best Performance:** Pure C++ implementations  
**Best Balance:** FasterAPI (Python + C++)  
**Best Ecosystem:** Python frameworks

**When to use what:**
- **Pure C++**: Maximum performance, <100µs latency requirements
- **FasterAPI**: I/O-bound apps, need Python ecosystem
- **FastAPI**: Development speed > raw performance

---

**Last Updated:** October 19, 2025  
**Platform:** M2 MacBook Pro, macOS 14.6  
**Python:** 3.13.7  
**Compiler:** clang (Apple LLVM) with -O3 -mcpu=native -flto
