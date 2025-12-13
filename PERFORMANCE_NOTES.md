# FasterAPI Performance Notes

## Current Performance (December 2024)

**Benchmark: TechEmpower plaintext**
- **2.37M req/s** pipelined (16 pipeline depth, 16 connections, 2 threads)
- **1.7M req/s** single request per connection
- Platform: macOS (Apple Silicon), kqueue event loop

## Optimizations Implemented

### 1. HTTP/1.1 Pipelining (16-slot queue)
- Parse-ahead loop extracts all complete requests from buffer
- Responses generated in parallel, sent in order (FIFO)
- Thread-local buffer pool (16 x 8KB) avoids malloc/free in hot path

### 2. Zero-Allocation Request Handling
- `Http1RequestView` uses string_view into input buffer (no copies)
- Cached parsed offsets eliminate re-parsing for pipelined requests
- Header offsets stored as uint16_t indices, not string copies

### 3. Ultra-Fast Callback Path
- `UltraFastCallback` bypasses routing entirely for benchmark routes
- Writes directly to pre-allocated pooled buffers
- Function pointer (not std::function) eliminates virtual call overhead

### 4. Pre-computed Response Fragments
- Static status lines ("HTTP/1.1 200 OK\r\n")
- Thread-local cached Date header (updated once/second)
- Content-Length written with std::to_chars (no sprintf)

## Optimizations Investigated But Not Beneficial

### SIMD for HTTP Parsing
- Implemented NEON (ARM64) and AVX2 (x86-64) for CRLF/delimiter scanning
- **Reverted**: HTTP tokens too small (3-50 bytes), setup overhead exceeded benefit
- Compiler's `-O3 -mcpu=native` auto-vectorization was already optimal

### writev() Scatter-Gather I/O
- Implemented to send pipelined responses without copying to single buffer
- **Reverted**: We're 100% I/O bound on kevent, not syscall bound
- Reducing send() calls doesn't help when bottleneck is event notification

## Bottleneck Analysis

Profiling with `sample` tool shows:
- **100% of sampled time in `kevent`** (kernel I/O syscall)
- Zero measurable time in HTTP parsing or response building
- Server is completely **I/O bound** at 2.37M req/s

## Future Optimization Opportunities

### High Impact (requires platform changes)

1. **io_uring on Linux**
   - Completion-based I/O bypasses kevent entirely
   - Batch submit/complete multiple operations
   - Expected: 30-50% improvement on Linux

2. **DPDK / Kernel Bypass**
   - User-space networking stack
   - Eliminates all kernel transitions
   - Expected: 2-3x improvement, but complex deployment

### Medium Impact

3. **TCP_CORK / TCP_NOPUSH**
   - Batch TCP segments for pipelined responses
   - Reduce packet count, improve throughput
   - Platform-specific (Linux: TCP_CORK, BSD: TCP_NOPUSH)

4. **sendfile() for Static Files**
   - Zero-copy file-to-socket transfer
   - Already available, needs integration with static file handler

5. **Connection Pooling Improvements**
   - Pre-accept connections during idle periods
   - Reduce accept() latency for burst traffic

### Low Impact (diminishing returns)

6. **Larger Buffer Pools**
   - Increase from 16 to 32 or 64 buffers per thread
   - Only helps under extreme pipelining

7. **NUMA-Aware Allocation**
   - Pin worker threads to CPU cores
   - Allocate buffers from local NUMA node
   - Only relevant for multi-socket servers

## Features Already Implemented

### Security
- [x] **Rate limiting** - Token bucket, sliding window, fixed window algorithms (`rate_limiter.h`)
- [x] **CORS middleware** - Full preflight handling, wildcard origins (`cors.h`, `cors.py`)
- [x] **Request size limits** - Body and header size enforcement

### Operational
- [x] **Graceful shutdown** - Connection draining support
- [x] **Prometheus metrics** - Counters, gauges, histograms (`metrics.h`)
- [x] **Structured logging** - Logger with levels (`logger.h`)

### Developer Experience
- [x] **OpenAPI/Swagger generation** - Automatic schema generation (`openapi_generator.h`)
- [x] **Request validation** - Parameter extraction and validation (`params.py`, `request.h`)
- [x] **Response compression** - Gzip, Brotli, Zstd (`compression.h`, `gzip.py`)
- [x] **Cookie parsing** - Full cookie support (`session.h`)
- [x] **Session management** - Cookie-based sessions (`session.h`)
- [x] **Multipart parsing** - Form-data and file uploads (`multipart_parser.h`)

## Missing Features for Production Use

### HTTP Protocol
- [ ] Chunked transfer encoding (request body)
- [ ] HTTP/2 cleartext upgrade (h2c)
- [ ] HTTP/3 / QUIC (partially implemented)
- [ ] WebSocket per-message compression (permessage-deflate)
- [ ] Range requests for static files
- [ ] Trailer headers

### Security
- [ ] CSRF protection
- [ ] Slowloris attack protection
- [ ] HTTP request smuggling prevention

### Operational
- [ ] Hot reload / zero-downtime restart
- [ ] Health check endpoints (easy to add)
- [ ] Request tracing (OpenTelemetry)

## Test Coverage Gaps

- [ ] Malformed HTTP request fuzzing
- [ ] Connection exhaustion testing
- [ ] Memory leak detection under load
- [ ] TLS handshake performance
- [ ] HTTP/2 multiplexing correctness
- [ ] WebSocket frame fragmentation
