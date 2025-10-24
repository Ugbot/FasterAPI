# FasterAPI HTTP/1 Benchmarks - Quick Start

## ✅ What's Available

### 1. C++ Benchmark Script (Working Now!)
**Run:** `./benchmarks/run_cpp_http1_benchmarks.sh [requests] [concurrent]`

Tests three C++ HTTP/1.1 server implementations:
- Threading (thread-per-connection)
- Async I/O (kqueue on macOS)
- libuv (production event loop)

**Examples:**
```bash
# Quick test (10K requests)
./benchmarks/run_cpp_http1_benchmarks.sh 10000 100

# Medium test (100K requests) - recommended
./benchmarks/run_cpp_http1_benchmarks.sh 100000 500

# Full 1M test (takes ~50s)
./benchmarks/run_cpp_http1_benchmarks.sh 1000000 1000
```

### 2. TechEmpower Benchmarks (Python)
**Run:** `./benchmarks/techempower/run_techempower_tests.sh`

Tests standard TechEmpower endpoints:
- `/json` - JSON serialization
- `/plaintext` - Minimum overhead
- `/db` - Database queries
- `/queries?queries=N` - Multiple queries
- `/updates?queries=N` - Database updates

**Note:** Requires wrk or ab (Apache Bench)

### 3. Manual Server Testing

Start any server manually and test with the Python client:

```bash
# Terminal 1: Start server
./build/benchmarks/1mrc_libuv_server

# Terminal 2: Run client
cd benchmarks/1mrc/client
python3 1mrc_client.py 1000000 1000

# Stop server
pkill -f 1mrc_libuv_server
```

## 📊 Recent Results (100K requests, 500 concurrent)

| Implementation | Throughput | Time | Accuracy |
|----------------|-----------|------|----------|
| **Threading** | 18,580 req/s | 5.38s | ✅ 100% |
| **Async I/O** | 18,517 req/s | 5.40s | ✅ 100% |
| **libuv** ⭐ | 18,828 req/s | 5.31s | ✅ 100% |

All tests: **Zero errors**, **perfect data integrity**

## 🔧 Quick Commands

```bash
# Check if servers are built
ls build/benchmarks/1mrc_*

# Rebuild if needed
cd build && ninja 1mrc_cpp_server 1mrc_libuv_server 1mrc_async_server

# Run benchmarks
./benchmarks/run_cpp_http1_benchmarks.sh 100000 500

# View results
ls -lt benchmarks/1mrc/client/logs/
```

## 📚 Documentation

- **Full Guide:** `benchmarks/HTTP1_BENCHMARK_GUIDE.md`
- **1MRC Details:** `benchmarks/1mrc/README.md`
- **TechEmpower:** `benchmarks/techempower/`

## 🎯 What Protocol?

**All benchmarks use HTTP/1.1 exclusively.**

The servers were specifically built for the 1 Million Request Challenge and TechEmpower Framework Benchmarks, both of which test HTTP/1.1 performance.

## 🚀 Next Steps

1. **Run full 1M test** to see true performance ceiling:
   ```bash
   ./benchmarks/run_cpp_http1_benchmarks.sh 1000000 1000
   ```

2. **Compare with other frameworks** using the official 1MRC implementations:
   ```bash
   cd benchmarks/1mrc/official/
   # See README for Go, Rust, Java Spring Boot tests
   ```

3. **Test with wrk** for more detailed metrics:
   ```bash
   # Start any HTTP server on port 8000
   ./build/benchmarks/1mrc_libuv_server &

   # Benchmark with wrk (if installed)
   wrk -t8 -c500 -d30s http://localhost:8000/stats
   ```

## ⚠️ Known Issues

- **Python TechEmpower server:** Currently has library dependency issues. Use the shell script directly or run C++ benchmarks.
- **Unified script:** The main `run_http1_benchmarks.sh` needs library fixes. Use `run_cpp_http1_benchmarks.sh` instead.

## 💡 Tips

- **Low numbers?** The Python client can become the bottleneck. Run with higher concurrency (1000-2000 workers) for better saturation.

- **Want to test TechEmpower?** The C++ servers don't implement TechEmpower endpoints. Use the Python server once library issues are resolved, or implement the endpoints in C++.

- **Comparing implementations?** Run each test 3 times and average the results for more stable numbers.

---

**Created:** 2025-10-24
**Status:** C++ benchmarks working, Python benchmarks need library fixes
