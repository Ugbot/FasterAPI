# 1 Million Request Challenge (1MRC) - FasterAPI Edition

FasterAPI's implementation of the [1 Million Request Challenge](https://github.com/Kavishankarks/1mrc) - processing **1,000,000 concurrent requests** with accuracy and speed.

## 📊 Official Challenge Repository

The official 1MRC repository by [Kavishankarks](https://github.com/Kavishankarks/1mrc) is included as a **git submodule** in `official/`:

```bash
# View official implementations (Go, Java Spring Boot, Rust)
cd official/
ls -la

# Go implementation
cd go-service && go run main.go

# Java Spring Boot
cd java-spring && mvn spring-boot:run

# Rust implementation  
cd rust-service && cargo run --release
```

**Official Benchmark Results:**
- **Go**: ~85,000 req/s (~50MB memory)
- **Java Spring Boot**: ~10,000-15,000 req/s (~200MB+ memory)
- **Rust**: ~120,000 req/s (~40MB memory)

See [official/README.md](official/README.md) for full details from the original repository.

---

## 🎯 Challenge Requirements

### Endpoints

#### POST /event
Accept event data:
```json
{
  "userId": "user_12345",
  "value": 499.5
}
```

#### GET /stats
Return aggregated statistics:
```json
{
  "totalRequests": 1000000,
  "uniqueUsers": 75000,
  "sum": 499500000.0,
  "avg": 499.5
}
```

### Success Criteria

- ✅ Process **1,000,000 requests** accurately
- ✅ Handle **hundreds/thousands concurrent requests per second**
- ✅ Maintain **thread safety** (no lost or double-counted requests)
- ✅ Optimize for **maximum throughput**
- ✅ Ensure **data integrity** under high concurrency

---

## 🚀 FasterAPI Implementations

We provide **6 different implementations** showcasing various approaches and technologies:

### C++ Implementations (High Performance)

Located in `cpp/`:

| Implementation | Performance | Description | Run |
|----------------|-------------|-------------|-----|
| **Threading** | ~85K req/s | Traditional multi-threading | `./build/benchmarks/1mrc_cpp_server` |
| **Async I/O** | ~120K req/s | kqueue/epoll/io_uring based | `./build/benchmarks/1mrc_async_server` |
| **libuv** | **~200K req/s** | Production event loop | `./build/benchmarks/1mrc_libuv_server` |

**Key Features:**
- Lock-free atomic operations
- Zero-copy data structures
- Minimal memory allocation
- Platform-optimized (kqueue on macOS, epoll on Linux)
- Sub-50MB memory footprint

### Python Implementations (Productivity)

Located in `python/`:

| Implementation | Performance | Description | Run |
|----------------|-------------|-------------|-----|
| **Standard** | ~12K req/s | Basic Python + threading | `python3 benchmarks/1mrc/python/1mrc_server.py` |
| **Async** | ~15K req/s | asyncio event loop | `python3 benchmarks/1mrc/python/1mrc_async_server.py` |
| **FasterAPI Native** | **~50K req/s** | C++ backend integration | `python3 benchmarks/1mrc/python/1mrc_fasterapi_native.py` |

**Key Features:**
- Thread-safe with Python locks
- asyncio for concurrency
- Integration with FasterAPI's C++ optimizations
- Easy to understand and modify

---

## 🏆 Performance Comparison

### All Implementations

| Implementation | Throughput | Memory | Language | Notes |
|----------------|-----------|--------|----------|-------|
| **FasterAPI libuv (C++)** | **200K req/s** | ~45MB | C++ | 🥇 **Best Performance** |
| **FasterAPI Async I/O** | 120K req/s | ~50MB | C++ | Platform-optimized |
| **Rust (official)** | 120K req/s | ~40MB | Rust | Official implementation |
| **FasterAPI Threading** | 85K req/s | ~50MB | C++ | Traditional approach |
| **Go (official)** | 85K req/s | ~50MB | Go | Official implementation |
| **FasterAPI Native (Python)** | 50K req/s | ~80MB | Python+C++ | Hybrid approach |
| **FasterAPI Async (Python)** | 15K req/s | ~100MB | Python | asyncio-based |
| **Java Spring (official)** | 10-15K req/s | ~200MB | Java | Official implementation |
| **FasterAPI Standard (Python)** | 12K req/s | ~100MB | Python | Basic implementation |

### Key Achievements

🎉 **FasterAPI C++ libuv: 200,000+ requests/second**
- **2.35x faster** than official Go implementation
- **1.67x faster** than Rust implementation
- **16x faster** than Java Spring Boot
- **100% accuracy** - All 1M requests counted correctly
- **Zero errors** - Perfect thread safety

---

## 🚀 Quick Start

### Option 1: C++ libuv (Recommended - Best Performance)

```bash
# Build (if not already built)
cd build
make 1mrc_libuv_server -j8
cd ..

# Start server
./build/benchmarks/1mrc_libuv_server &
SERVER_PID=$!

# Wait for server to start
sleep 1

# Run test
python3 benchmarks/1mrc/client/1mrc_client.py

# Results will be displayed and logged
kill $SERVER_PID
```

### Option 2: Python Standard (Easiest)

```bash
# Start server
python3 benchmarks/1mrc/python/1mrc_server.py &
SERVER_PID=$!

# Wait for server to start
sleep 1

# Run test
python3 benchmarks/1mrc/client/1mrc_client.py

# Stop server
kill $SERVER_PID
```

### Option 3: Compare with Official Implementations

```bash
# Go implementation
cd benchmarks/1mrc/official/go-service
go run main.go &
GO_PID=$!
sleep 1
go run test_client.go
kill $GO_PID

# Java Spring Boot
cd ../java-spring
mvn spring-boot:run &
JAVA_PID=$!
sleep 5
mvn exec:java
kill $JAVA_PID
```

---

## 📁 Directory Structure

```
1mrc/
├── README.md                  # This file
├── GUIDE.md                   # Additional guide
│
├── cpp/                       # C++ implementations
│   ├── 1mrc_cpp_server.cpp    # Threading-based
│   ├── 1mrc_async_server.cpp  # Async I/O (kqueue/epoll)
│   └── 1mrc_libuv_server.cpp  # libuv event loop
│
├── python/                    # Python implementations
│   ├── 1mrc_server.py         # Standard Python
│   ├── 1mrc_async_server.py   # asyncio-based
│   └── 1mrc_fasterapi_native.py # FasterAPI C++ integration
│
├── client/                    # Test clients
│   └── 1mrc_client.py         # Python async client (1M requests)
│
├── results/                   # Benchmark results
│   ├── 1MRC_RESULTS.md        # Main results
│   ├── 1MRC_ALL_VERSIONS_RESULTS.md
│   ├── 1MRC_COMPARISON.md
│   └── 1MRC_ASYNC_ANALYSIS.md
│
└── official/                  # Git submodule
    ├── go-service/            # Official Go implementation
    ├── java-spring/           # Official Java Spring Boot
    ├── rust-service/          # Official Rust implementation
    └── README.md              # Official challenge README
```

---

## 🔧 Building & Running

### Build All C++ Servers

```bash
cd build
cmake ..
make 1mrc_cpp_server 1mrc_async_server 1mrc_libuv_server -j8
cd ..
```

### Run Individual Implementations

#### C++ Threading
```bash
./build/benchmarks/1mrc_cpp_server &
```

#### C++ Async I/O  
```bash
./build/benchmarks/1mrc_async_server &
```

#### C++ libuv
```bash
./build/benchmarks/1mrc_libuv_server &
```

#### Python Standard
```bash
python3 benchmarks/1mrc/python/1mrc_server.py &
```

#### Python Async
```bash
python3 benchmarks/1mrc/python/1mrc_async_server.py &
```

#### Python FasterAPI Native
```bash
python3 benchmarks/1mrc/python/1mrc_fasterapi_native.py &
```

### Run Test Client

```bash
# Default: 1M requests, 1000 concurrent
python3 benchmarks/1mrc/client/1mrc_client.py

# Custom: 500K requests, 500 concurrent
python3 benchmarks/1mrc/client/1mrc_client.py 500000 500
```

---

## 📊 Sample Output

```
Starting 1MRC test with 1000000 requests and 1000 concurrent workers
Completed: 100000/1000000 (187543.2 req/s)
Completed: 200000/1000000 (195821.1 req/s)
Completed: 300000/1000000 (198234.5 req/s)
...
Completed: 1000000/1000000 (201456.7 req/s)

=== Test Results ===
Total time: 4.963s
Requests per second: 201456.71
Errors: 0

=== Server Stats ===
Total Requests: 1000000
Unique Users: 75000
Sum: 499500000.00
Average: 499.50

✅ SUCCESS: All requests processed correctly!
```

---

## 🏗️ Implementation Details

### C++ libuv (Best Performance)

**Architecture:**
- libuv event loop for async I/O
- Lock-free atomic operations for counters
- Hash table for unique user tracking
- Zero-copy response building

**Key Optimizations:**
- Preallocated buffers
- Efficient HTTP parsing
- Minimal memory allocation
- Platform-optimized event notification

**Code Structure:**
```cpp
// Atomic counters
std::atomic<uint64_t> total_requests{0};
std::atomic<double> sum{0.0};
std::unordered_set<std::string> users;  // Protected by mutex
std::mutex users_mutex;

// libuv HTTP server
uv_tcp_t server;
uv_loop_t* loop = uv_default_loop();
```

### Python Async (Simplicity)

**Architecture:**
- asyncio event loop
- aiohttp for async HTTP
- Python locks for thread safety
- Dict for user tracking

**Code Structure:**
```python
total_requests = 0
sum_value = 0.0
users = set()
lock = asyncio.Lock()

async def handle_event(request):
    async with lock:
        # Thread-safe updates
        total_requests += 1
        sum_value += value
        users.add(user_id)
```

---

## 📈 Benchmarking Methodology

### Test Configuration
- **Total Requests**: 1,000,000
- **Concurrent Workers**: 1,000
- **Request Pattern**: Deterministic (user_0 to user_74999, values 0.0 to 999.0)
- **Expected Sum**: 499,500,000.0
- **Expected Average**: 499.5
- **Expected Unique Users**: 75,000

### Client Configuration
- **HTTP Client**: aiohttp with connection pooling
- **Keep-Alive**: Enabled
- **Connection Limit**: 1,000 concurrent
- **Progress Reporting**: Every 100K requests

### Validation
- ✅ All 1M requests received
- ✅ Correct sum (499,500,000.0)
- ✅ Correct average (499.5)
- ✅ Correct unique users (75,000)
- ✅ Zero errors
- ✅ Zero data loss

---

## 🎯 Performance Tips

### For Maximum Throughput

1. **Use C++ libuv implementation** (200K req/s)
2. **Increase system limits**:
   ```bash
   ulimit -n 65536  # File descriptors
   ```
3. **Optimize network stack**:
   ```bash
   # macOS
   sudo sysctl -w kern.ipc.somaxconn=4096
   sudo sysctl -w net.inet.tcp.msl=1000
   ```

### For Development/Testing

1. **Use Python async** (15K req/s, easy to modify)
2. **Adjust concurrency**:
   ```bash
   python3 client/1mrc_client.py 100000 100  # Smaller test
   ```

---

## 📊 Results & Analysis

Detailed results available in `results/`:

- **[1MRC_RESULTS.md](results/1MRC_RESULTS.md)** - Main benchmark results
- **[1MRC_ALL_VERSIONS_RESULTS.md](results/1MRC_ALL_VERSIONS_RESULTS.md)** - All implementations compared
- **[1MRC_COMPARISON.md](results/1MRC_COMPARISON.md)** - vs Go, Java, Rust (official)
- **[1MRC_ASYNC_ANALYSIS.md](results/1MRC_ASYNC_ANALYSIS.md)** - Async I/O deep dive

---

## 🔬 Technical Insights

### Why libuv is Fastest

1. **Event loop efficiency** - Optimized for high-throughput I/O
2. **Platform-specific backends**:
   - macOS/BSD: kqueue
   - Linux: epoll (with io_uring option)
   - Windows: IOCP
3. **Zero-copy operations** - Minimal data movement
4. **Efficient memory management** - Object pooling

### Why Python is Slower

1. **GIL overhead** - Global Interpreter Lock serialization
2. **Object allocation** - Python objects are expensive
3. **Lock contention** - Thread safety requires locks
4. **Interpretation overhead** - Bytecode execution

### FasterAPI Hybrid Approach

- **C++ for I/O** (libuv event loop): 200K req/s
- **Python for logic** (if needed): Acceptable for I/O-bound
- **Best of both worlds**: Performance + Productivity

---

## 🏅 Comparison with Official Implementations

| Metric | FasterAPI (libuv) | Go | Rust | Java Spring |
|--------|-------------------|-----|------|-------------|
| **Throughput** | **200K req/s** 🥇 | 85K | 120K 🥈 | 10-15K |
| **Memory** | 45MB | 50MB | 40MB 🥇 | 200MB+ |
| **Startup** | Instant | Instant | Instant | ~2 sec |
| **CPU Efficiency** | Excellent | High | Excellent | Medium |
| **Code Complexity** | Medium | Low 🥇 | Medium | High |
| **Ecosystem** | C++/Python | Go | Rust | Java |

**Verdict**: FasterAPI's C++ libuv implementation achieves **top performance** while maintaining reasonable complexity.

---

## 🚀 Next Steps

1. **Try all implementations** - Compare performance on your hardware
2. **Read the analysis** - See [results/1MRC_COMPARISON.md](results/1MRC_COMPARISON.md)
3. **Contribute** - Add more optimizations or new language implementations
4. **Compare** - Check the official repository in `official/`

---

## 📚 References

- **Official Challenge**: https://github.com/Kavishankarks/1mrc
- **libuv Documentation**: https://docs.libuv.org/
- **FasterAPI Benchmarks**: [../BENCHMARK_RESULTS.md](../../BENCHMARK_RESULTS.md)
- **Python Overhead Analysis**: [../../PYTHON_OVERHEAD_ANALYSIS.md](../../PYTHON_OVERHEAD_ANALYSIS.md)

---

**Challenge accepted and conquered! 🚀 FasterAPI achieves 200K+ req/s! 🏆**
