# C++ HTTP Server Optimizations

This document describes the performance optimizations implemented in FasterAPI's pure C++ HTTP server, which achieves **190,000+ requests/second** on a single machine.

## Benchmark Results

Benchmarked with `wrk` (industry standard HTTP benchmark tool):

| Test | FasterAPI (C++) | FastAPI (uvicorn) | Speedup |
|------|-----------------|-------------------|---------|
| JSON (256 conn) | **191,213 req/s** | 11,413 req/s | **16.8x** |
| Plaintext (256 conn) | **184,681 req/s** | 12,080 req/s | **15.3x** |
| High Concurrency (512 conn) | **188,539 req/s** | 9,121 req/s | **20.7x** |
| HTTP Pipelining (16 req) | **193,056 req/s** | N/A | - |

| Latency | FasterAPI | FastAPI | Improvement |
|---------|-----------|---------|-------------|
| Average | 1.3ms | 22.9ms | **17.6x lower** |
| p99 | ~13ms | ~200ms | **15x lower** |

Test configuration:
- macOS (Apple Silicon M-series)
- wrk: 8-16 threads, 256-512 connections, 10s duration
- HTTP/1.1 with keep-alive
- FasterAPI: 10 C++ workers with kqueue
- FastAPI: Uvicorn single worker

## Architecture Overview

```
                    ┌─────────────────────────────────────────┐
                    │            FasterAPI Server             │
                    └─────────────────────────────────────────┘
                                       │
                    ┌──────────────────┴──────────────────┐
                    │         SO_REUSEPORT Listener       │
                    │    (kernel load-balances to workers)│
                    └──────────────────┬──────────────────┘
                                       │
        ┌──────────┬──────────┬────────┴────────┬──────────┬──────────┐
        │          │          │                 │          │          │
   ┌────┴────┐┌────┴────┐┌────┴────┐      ┌────┴────┐┌────┴────┐┌────┴────┐
   │Worker 0 ││Worker 1 ││Worker 2 │ ...  │Worker 7 ││Worker 8 ││Worker 9 │
   │ kqueue  ││ kqueue  ││ kqueue  │      │ kqueue  ││ kqueue  ││ kqueue  │
   └─────────┘└─────────┘└─────────┘      └─────────┘└─────────┘└─────────┘
```

## Key Optimizations

### 1. SO_REUSEPORT Multi-Worker Architecture

**Problem:** Single-threaded servers can't utilize multiple CPU cores.

**Solution:** Use `SO_REUSEPORT` to allow multiple worker threads to bind to the same port. The kernel distributes incoming connections across workers.

```cpp
// Enable SO_REUSEPORT on the listening socket
int enable = 1;
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
```

**Benefits:**
- True parallelism across CPU cores
- No userspace load balancing overhead
- Kernel handles connection distribution efficiently
- Each worker has its own event loop (no shared state)

**Configuration:** Default 10 workers, tunable based on CPU cores.

---

### 2. Edge-Triggered kqueue Event Loop

**Problem:** Level-triggered events cause redundant wake-ups.

**Solution:** Use edge-triggered (`EV_CLEAR`) kqueue events that only fire when state changes.

```cpp
struct kevent ev;
EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
kevent(kq, &ev, 1, nullptr, 0, nullptr);
```

**Benefits:**
- Fewer syscalls (only wake on new data)
- No thundering herd on multi-event scenarios
- Better CPU cache utilization

**Trade-off:** Must drain all available data when event fires.

---

### 3. Lock-Free Logging

**Problem:** Traditional logging uses mutexes, causing contention under load.

**Before (mutex-based):**
```cpp
void Logger::log(...) {
    std::lock_guard<std::mutex> lock(output_mutex_);  // Contention!
    fprintf(output_file_, ...);
    fflush(output_file_);
}
```

**After (lock-free):**
```cpp
void log(...) noexcept {
    // Thread-local buffer - no contention
    thread_local char buffer[4096];
    
    // Format message locally
    int len = snprintf(buffer, sizeof(buffer), ...);
    
    // Single atomic write syscall (< PIPE_BUF is atomic)
    ::write(output_fd_.load(std::memory_order_relaxed), buffer, len);
}
```

**Benefits:**
- No mutex contention between workers
- Thread-local formatting (no shared state)
- Single `write()` syscall (atomic for < 4KB messages)
- No `fflush()` overhead

**Impact:** 4.5x throughput improvement (34K → 154K req/s)

---

### 4. TCP_NODELAY (Disable Nagle's Algorithm)

**Problem:** Nagle's algorithm batches small writes, adding latency.

**Solution:** Disable Nagle's algorithm for low-latency responses.

```cpp
int enable = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
```

**Benefits:**
- Responses sent immediately
- Lower latency for small responses
- Better for request-response protocols

**Trade-off:** Slightly more packets for large responses (usually negligible).

---

### 5. HTTP/1.1 Keep-Alive Connection Reuse

**Problem:** Opening a new TCP connection per request is expensive.

**Solution:** Reuse connections for multiple requests (HTTP/1.1 keep-alive).

```cpp
void Http1Connection::reset_for_next_request() noexcept {
    // Preserve connection, reset request state
    input_buffer_.erase(input_buffer_.begin(), 
                        input_buffer_.begin() + bytes_consumed_);
    output_buffer_.clear();
    current_request_ = HTTP1Request{};  // Reset headers, etc.
    state_ = Http1State::READING_REQUEST;
    requests_served_++;
}
```

**Benefits:**
- Amortize TCP handshake cost across many requests
- Reduce TIME_WAIT socket accumulation
- Lower latency for subsequent requests

**Bug fixed:** Header count was accumulating across requests, causing connections to close after ~50 requests. Fixed by resetting `current_request_` struct.

---

### 6. Zero-Copy Where Possible

**Problem:** Copying data between buffers wastes CPU cycles.

**Solution:** Use `string_view` and direct buffer access where possible.

```cpp
// Parser stores views into input buffer (no copy)
struct HTTP1Request {
    std::string_view method_str;
    std::string_view url;
    struct Header {
        std::string_view name;
        std::string_view value;
    } headers[MAX_HEADERS];
};
```

**Benefits:**
- Reduced memory bandwidth
- Better CPU cache utilization
- Fewer allocations in hot path

---

### 7. Pre-allocated Buffers

**Problem:** Dynamic allocation in hot paths is slow.

**Solution:** Pre-allocate buffers per connection.

```cpp
class Http1Connection {
    std::vector<char> input_buffer_;   // Pre-reserved
    std::vector<char> output_buffer_;  // Pre-reserved
    
    Http1Connection(int fd) {
        input_buffer_.reserve(8192);   // Typical request size
        output_buffer_.reserve(16384); // Typical response size
    }
};
```

**Benefits:**
- Fewer allocations during request processing
- Predictable memory usage
- Better cache locality

---

### 8. Efficient HTTP Parsing

**Problem:** Naive parsing with string operations is slow.

**Solution:** State machine parser with minimal allocations.

```cpp
enum class ParseState {
    METHOD,
    URL,
    VERSION,
    HEADER_NAME,
    HEADER_VALUE,
    BODY,
    COMPLETE
};

// Single pass through input buffer
ParseResult parse(const char* data, size_t len) {
    while (pos < len) {
        switch (state_) {
            case ParseState::METHOD:
                // Find space, extract method
                break;
            // ... etc
        }
    }
}
```

**Benefits:**
- Single pass parsing
- No regex overhead
- Minimal string allocations

---

## Profiling Results

CPU profiling with macOS `sample` tool shows:

```
100% time in kevent (I/O wait)
  0% time in userspace request processing
```

**Interpretation:**
- Server is completely I/O bound
- Request processing is sub-millisecond (doesn't show in 1ms sampling)
- No userspace bottlenecks remain

---

## Configuration Recommendations

### Production Settings

```cpp
// Optimal for most workloads
App::Config config;
config.workers = std::thread::hardware_concurrency();  // Match CPU cores
config.pure_cpp_mode = true;  // No Python overhead

// Logger set to WARN (minimal logging in production)
Logger::instance().set_level(LogLevel::WARN);
```

### System Tuning (Linux)

```bash
# Increase file descriptor limits
ulimit -n 1000000

# Tune TCP settings
sysctl -w net.core.somaxconn=65535
sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sysctl -w net.core.netdev_max_backlog=65535

# Enable TCP Fast Open
sysctl -w net.ipv4.tcp_fastopen=3
```

### System Tuning (macOS)

```bash
# Increase file descriptor limits
sudo launchctl limit maxfiles 1000000 1000000
ulimit -n 1000000

# Tune TCP settings
sudo sysctl -w kern.ipc.somaxconn=65535
```

---

## Potential Future Optimizations

These provide diminishing returns but may help in extreme cases:

1. **io_uring on Linux** - More efficient than epoll/kqueue for high connection counts
2. **Object pools for headers** - Avoid `unordered_map` allocation per request
3. **`string_view` in callbacks** - Avoid string copies to user handlers
4. **Batch event processing** - Process multiple events before returning to kevent
5. **Huge pages** - Reduce TLB misses for large connection counts
6. **CPU pinning** - Pin workers to specific cores for better cache locality

---

## Bugs Fixed During Optimization

### 1. LOG_DEBUG Mutex Contention
- **Symptom:** ~100 req/s (240x slower than expected)
- **Cause:** LOG_DEBUG macro called `log()` unconditionally, acquiring mutex
- **Fix:** Check log level before calling `log()`

### 2. File Descriptor Leak
- **Symptom:** "Too many open files" after sustained load
- **Cause:** `remove_fd()` didn't call `::close(fd)`
- **Fix:** Added `::close(fd)` after every `remove_fd()` call

### 3. 50-Request Keep-Alive Limit
- **Symptom:** Connections closed after exactly 50 requests
- **Cause:** `header_count` accumulated across requests (2 headers × 50 = 100 = MAX_HEADERS)
- **Fix:** Reset `current_request_` struct in `reset_for_next_request()`

### 4. Mutex-Based Logger
- **Symptom:** 34K req/s (good but not great)
- **Cause:** Even with level check, `fprintf`/`fflush` with mutex caused contention
- **Fix:** Lock-free logger with thread-local buffers and direct `write()` syscall
- **Impact:** 34K → 154K req/s (4.5x improvement)

---

## Summary

FasterAPI's C++ HTTP server achieves **170,000+ req/s** through:

| Optimization | Impact |
|--------------|--------|
| SO_REUSEPORT multi-worker | Linear scaling with cores |
| Edge-triggered kqueue | Fewer syscalls |
| Lock-free logging | 4.5x throughput |
| TCP_NODELAY | Lower latency |
| HTTP/1.1 keep-alive | Connection reuse |
| Zero-copy parsing | Reduced allocations |
| Pre-allocated buffers | Predictable performance |

The server is now **I/O bound** (all time in `kevent`), meaning userspace code is not the bottleneck. Further improvements require kernel/hardware optimizations.
