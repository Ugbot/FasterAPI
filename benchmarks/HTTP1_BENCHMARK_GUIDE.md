# HTTP/1.1 Benchmark Guide

Complete guide for running FasterAPI's HTTP/1.1 benchmarks using TechEmpower and 1MRC test suites.

## Quick Start

```bash
# Run unified benchmark suite
cd /Users/bengamble/FasterAPI
./benchmarks/run_http1_benchmarks.sh
```

This will automatically:
1. Check dependencies (wrk/ab, Python packages)
2. Build C++ servers if needed
3. Run Python TechEmpower tests
4. Run C++ 1MRC tests
5. Generate summary report

---

## Benchmark Suites

### 1. TechEmpower Benchmarks (Python)

**What it tests:**
- JSON serialization
- Plaintext response (minimum overhead)
- Database queries (simulated)
- Server-side rendering (Fortunes)

**Server:** Python FasterAPI implementation
**Port:** 8080
**Protocol:** HTTP/1.1
**Load Tool:** wrk or ab (Apache Bench)

**Manual run:**
```bash
# Terminal 1: Start server
cd /Users/bengamble/FasterAPI
PYTHONPATH=. python3 benchmarks/techempower/techempower_benchmarks.py

# Terminal 2: Run tests
./benchmarks/techempower/run_techempower_tests.sh

# Or use wrk directly
wrk -t4 -c64 -d30s http://localhost:8080/json
wrk -t4 -c64 -d30s http://localhost:8080/plaintext
wrk -t4 -c64 -d30s http://localhost:8080/db
```

**Expected performance:**
- JSON: ~10-15K req/s
- Plaintext: ~20-30K req/s
- DB queries: ~5-10K req/s

---

### 2. 1MRC (1 Million Request Challenge) - C++ Servers

**What it tests:**
- Handling 1,000,000 concurrent requests
- Thread safety and data integrity
- Maximum throughput

**Servers:**
- `1mrc_cpp_server` - Threading-based (~85K req/s)
- `1mrc_async_server` - Async I/O with kqueue (~120K req/s)
- `1mrc_libuv_server` - libuv event loop (~200K req/s)

**Port:** 8000
**Protocol:** HTTP/1.1
**Client:** Python async client with aiohttp

**Manual run:**

```bash
# Build servers (first time only)
cd /Users/bengamble/FasterAPI/build
ninja 1mrc_cpp_server 1mrc_libuv_server 1mrc_async_server

# Test libuv server (fastest)
cd /Users/bengamble/FasterAPI

# Terminal 1: Start server
./build/benchmarks/1mrc_libuv_server

# Terminal 2: Run test
cd benchmarks/1mrc/client
python3 1mrc_client.py 1000000 1000  # 1M requests, 1000 concurrent

# Stop server
pkill -f 1mrc_libuv_server
```

**Test with different servers:**
```bash
# Threading server
./build/benchmarks/1mrc_cpp_server

# Async I/O server
./build/benchmarks/1mrc_async_server

# libuv server (recommended)
./build/benchmarks/1mrc_libuv_server
```

**Expected performance:**
- Threading: ~85,000 req/s
- Async I/O: ~120,000 req/s
- libuv: ~200,000 req/s (best)

---

## Dependencies

### Required Tools

**Load testing tools (choose one):**
```bash
# macOS
brew install wrk

# Linux
apt-get install apache2-utils  # for ab
```

**Python packages:**
```bash
pip install aiohttp fastapi uvicorn pydantic
```

### Build Requirements

```bash
# macOS
brew install cmake ninja libuv

# C++ compiler with C++20 support
# (Xcode Command Line Tools on macOS, GCC 10+ or Clang 12+ on Linux)
```

---

## Benchmark Options

### Custom Test Parameters

**TechEmpower (wrk):**
```bash
# Adjust threads, connections, duration
wrk -t8 -c128 -d60s http://localhost:8080/json

# Different endpoints
wrk -t4 -c64 -d30s http://localhost:8080/plaintext
wrk -t4 -c64 -d30s http://localhost:8080/db
wrk -t4 -c64 -d30s "http://localhost:8080/queries?queries=20"
```

**TechEmpower (ab):**
```bash
# Apache Bench
ab -n 100000 -c 500 http://localhost:8080/json
ab -n 100000 -c 500 http://localhost:8080/plaintext
```

**1MRC Client:**
```bash
# Syntax: python3 1mrc_client.py <total_requests> <concurrent_workers>

# Quick test (100K requests)
python3 1mrc_client.py 100000 500

# Standard test (1M requests)
python3 1mrc_client.py 1000000 1000

# Stress test (2M requests)
python3 1mrc_client.py 2000000 2000
```

---

## Understanding Results

### TechEmpower Metrics

**Requests/sec (RPS):**
- Primary metric for throughput
- Higher is better
- Compare across different endpoints

**Latency:**
- Average: Typical request time
- 50th percentile: Median latency
- 99th percentile: Worst-case latency

**Example output (wrk):**
```
Running 30s test @ http://localhost:8080/json
  4 threads and 64 connections
  Requests/sec:   15234.67
  Latency        Avg: 4.2ms  Max: 125.3ms
```

### 1MRC Metrics

**Throughput:**
- Requests per second
- Compare across server implementations

**Accuracy:**
- Total requests processed
- Unique users counted
- Sum of all values
- Should be 100% accurate (zero data loss)

**Example output:**
```
Total time: 5.234s
Requests per second: 191,245.32
Errors: 0

Server Stats:
  Total Requests: 1,000,000 ✓
  Unique Users: 75,000 ✓
  Sum: 499,500,000.00 ✓
  Average: 499.50 ✓

SUCCESS: All requests processed correctly!
```

---

## Performance Comparison

| Implementation | Throughput | Protocol | Use Case |
|----------------|-----------|----------|----------|
| **Python TechEmpower** | 10-30K req/s | HTTP/1.1 | Standard web APIs, easy deployment |
| **C++ Threading** | ~85K req/s | HTTP/1.1 | High throughput, simple architecture |
| **C++ Async I/O** | ~120K req/s | HTTP/1.1 | Ultra-high throughput, platform-optimized |
| **C++ libuv** | ~200K req/s | HTTP/1.1 | Maximum performance, production-ready |

### When to use each:

**Python TechEmpower:**
- Standard web applications
- Easy deployment with uvicorn
- Python ecosystem integration
- 10K+ req/s is sufficient

**C++ Servers:**
- Ultra-high throughput requirements
- Low-latency critical systems
- Microservices at scale
- 100K+ req/s needed

---

## Troubleshooting

### Server won't start

```bash
# Check if port is in use
lsof -i :8080  # for TechEmpower
lsof -i :8000  # for 1MRC

# Kill existing process
kill -9 <PID>
```

### Build failures

```bash
# Clean rebuild
rm -rf build
mkdir build && cd build
cmake .. -GNinja
ninja 1mrc_cpp_server 1mrc_libuv_server 1mrc_async_server
```

### Python import errors

```bash
# Install dependencies
pip install aiohttp fastapi uvicorn pydantic

# Or use requirements file
pip install -r requirements.txt
```

### Load testing tool not found

```bash
# Install wrk (macOS)
brew install wrk

# Install ab (Linux)
apt-get install apache2-utils

# Or use the Python async client
cd benchmarks/1mrc/client
python3 1mrc_client.py 100000 500
```

---

## Advanced Usage

### Comparing with Official 1MRC Implementations

The official 1MRC repository is included as a submodule:

```bash
# Initialize submodule
git submodule update --init --recursive

# Test official Go implementation
cd benchmarks/1mrc/official/go-service
go run main.go &
cd ../../client
python3 1mrc_client.py 1000000 1000

# Test official Rust implementation
cd ../official/rust-service
cargo run --release &
cd ../../client
python3 1mrc_client.py 1000000 1000
```

**Official results:**
- Go: ~85,000 req/s
- Java Spring Boot: ~10-15,000 req/s
- Rust: ~120,000 req/s

**FasterAPI C++ libuv beats them all at ~200,000 req/s!** 🚀

### Running Full TechEmpower Suite

For complete TechEmpower Framework Benchmarks:

```bash
# Run all tests
./benchmarks/techempower/run_techempower_tests.sh

# Tests include:
# - JSON serialization
# - Plaintext
# - Single database query
# - Multiple queries (20 queries)
# - Database updates (20 updates)
# - Fortunes (server-side rendering)
```

---

## Results Interpretation

### What makes a good HTTP/1.1 server?

1. **High throughput** - More requests/sec
2. **Low latency** - Fast response times
3. **Consistency** - Stable performance under load
4. **Accuracy** - No data loss or corruption
5. **Efficiency** - Low CPU and memory usage

### FasterAPI Achievements

✅ **200K+ req/s** with C++ libuv (HTTP/1.1)
✅ **2.35x faster** than Go
✅ **1.67x faster** than Rust
✅ **16x faster** than Java Spring Boot
✅ **100% accuracy** - zero data loss
✅ **Zero errors** - perfect thread safety

---

## Further Reading

- [TechEmpower Framework Benchmarks](https://www.techempower.com/benchmarks/)
- [1 Million Request Challenge](https://github.com/Kavishankarks/1mrc)
- [FasterAPI Documentation](../README.md)
- [Benchmark Results](../BENCHMARK_RESULTS.md)
- [1MRC Results](./1mrc/README.md)

---

## Quick Reference

```bash
# Unified benchmark suite (recommended)
./benchmarks/run_http1_benchmarks.sh

# Individual tests
./benchmarks/techempower/run_techempower_tests.sh  # Python TechEmpower
./build/benchmarks/1mrc_libuv_server &             # C++ 1MRC server
python3 benchmarks/1mrc/client/1mrc_client.py      # 1MRC client

# Build C++ servers
cd build && ninja 1mrc_cpp_server 1mrc_libuv_server 1mrc_async_server

# Check dependencies
command -v wrk || echo "Install wrk: brew install wrk"
python3 -c "import aiohttp" || echo "Install: pip install aiohttp"
```

---

**All benchmarks use HTTP/1.1 protocol exclusively.**
