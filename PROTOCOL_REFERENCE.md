# FasterAPI Protocol Implementation - Quick Reference

## File Structure Map

### HTTP/1.x
- **Parser**: `/Users/bengamble/FasterAPI/src/cpp/http/http1_parser.h` (247 lines)
  - `HTTP1Method` enum
  - `HTTP1Request` struct with string_view headers
  - `HTTP1Parser` state machine class
- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/http1_parser.cpp` (~300 lines)
  - `parse()` - Main entry point
  - `parse_method()`, `parse_url()`, `parse_version()`, `parse_header_field()`, `parse_header_value()`

- **Connection Handler**: `/Users/bengamble/FasterAPI/src/cpp/http/http1_connection.h` (241 lines)
  - `Http1Response` struct
  - `Http1Connection` class with state machine
- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/http1_connection.cpp`
  - `process_input()` - Parse request
  - `get_output()` / `commit_output()` - Response sending
  - `reset_for_next_request()` - Keep-alive

### HTTP/2

#### Frame Layer
- **Header**: `/Users/bengamble/FasterAPI/src/cpp/http/http2_frame.h` (350 lines)
  - All 10 FrameType enums
  - FrameFlags namespace (all frame-specific flags)
  - ErrorCode enum (RFC 7540 Section 7)
  - SettingsId enum
  - Parsing functions: `parse_*_frame()` (one per frame type)
  - Serialization functions: `write_*_frame()` (one per frame type)

- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/http2_frame.cpp` (546 lines)
  - Byte order helpers (read/write uint16/24/32/64)
  - All frame parsing implementations
  - All frame writing implementations
  - CONNECTION_PREFACE constant

#### HPACK Compression
- **Header**: `/Users/bengamble/FasterAPI/src/cpp/http/hpack.h` (327 lines)
  - `HPACKHeader` struct
  - `HPACKStaticTable` class - 61 pre-defined headers
  - `HPACKDynamicTable` class - Circular buffer, 128 entries max, 4096 bytes default
  - `HPACKDecoder` class - HPACK decompression
  - `HPACKEncoder` class - HPACK compression

- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/hpack.cpp` (586 lines)
  - Static table data (61 entries)
  - Dynamic table management
  - Integer encoding/decoding
  - String encoding/decoding
  - Huffman compression/decompression

- **Huffman Support**: `/Users/bengamble/FasterAPI/src/cpp/http/huffman.h` / `.cpp`
  - Huffman coding for header value compression

#### Streams
- **Header**: `/Users/bengamble/FasterAPI/src/cpp/http/http2_stream.h` (320 lines)
  - `StreamState` enum (IDLE, OPEN, HALF_CLOSED_LOCAL/REMOTE, CLOSED, RESERVED_*)
  - `Http2Stream` class - Single stream with state machine and flow control
  - `StreamManager` class - Manages all streams for a connection

- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/http2_stream.cpp`
  - State transitions
  - Flow control window management
  - Header/body storage

#### Connection
- **Header**: `/Users/bengamble/FasterAPI/src/cpp/http/http2_connection.h` (314 lines)
  - `ConnectionSettings` struct
  - `BufferPool<Size, PoolSize>` template - Zero-allocation buffer management
  - `PooledBuffer<Size, PoolSize>` RAII wrapper
  - `ConnectionState` enum
  - `Http2Connection` class

- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/http2_connection.cpp` (537 lines)
  - `process_input()` - Main frame processing
  - All frame handlers (settings, headers, data, window_update, ping, rst_stream, goaway)
  - `send_response()` - Complete response with HPACK encoding
  - `send_rst_stream()` / `send_goaway()` - Control frames
  - Buffer pool management
  - Output queueing

#### Server Push
- **Header**: `/Users/bengamble/FasterAPI/src/cpp/http/h2_server_push.h` (183 lines)
  - `PushPromise` struct
  - `PushRules` class - Configure what to push
  - `ServerPush` class - Push manager

- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/h2_server_push.cpp`
  - PUSH_PROMISE frame building
  - Push statistics

#### Server
- **Header**: `/Users/bengamble/FasterAPI/src/cpp/http/http2_server.h` (134 lines)
  - `Http2ServerConfig` struct
  - `Http2Server` class - Server with TLS and Python integration

- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/http2_server.cpp`
  - Server startup and connection handling
  - Integration with event loop
  - Python callback invocation

### WebSocket

- **Parser Header**: `/Users/bengamble/FasterAPI/src/cpp/http/websocket_parser.h` (235 lines)
  - `OpCode` enum (CONTINUATION, TEXT, BINARY, CLOSE, PING, PONG)
  - `CloseCode` enum (1000, 1001, etc.)
  - `FrameHeader` struct - FIN, RSV, opcode, mask, payload_length, masking_key
  - `FrameParser` class - Frame parsing state machine
  - `HandshakeUtils` class - SHA-1 and Base64 for handshake

- **Parser Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/websocket_parser.cpp` (~400 lines)
  - `parse_frame()` - State machine with READING_HEADER, READING_PAYLOAD_LENGTH_*, READING_MASKING_KEY, READING_PAYLOAD states
  - `unmask()` - Optimized 8-byte XOR unmasking
  - `build_frame()` - Serialize frame
  - `build_close_frame()` / `parse_close_payload()`
  - `validate_utf8()`
  - `compute_accept_key()` - Sec-WebSocket-Accept calculation
  - `validate_upgrade_request()`

- **Connection Header**: `/Users/bengamble/FasterAPI/src/cpp/http/websocket.h` (211 lines)
  - `WebSocketConnection::Config` - compression, max_message_size, ping/pong intervals
  - `WebSocketConnection` class - Connection management

- **Connection Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/websocket.cpp` (~350 lines)
  - `send_text()` / `send_binary()`
  - `send_ping()` / `send_pong()`
  - `close()` - Close handshake
  - `handle_frame()` - Process incoming frame
  - Fragmentation handling
  - Callbacks: on_text_message, on_binary_message, on_close, on_error, on_ping, on_pong

### Server-Sent Events (SSE)

- **Header**: `/Users/bengamble/FasterAPI/src/cpp/http/sse.h` (220 lines)
  - `SSEConnection` class - Per-connection manager
  - `SSEEndpoint` class - Endpoint manager with connection pool

- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/sse.cpp` (~230 lines)
  - `send()` - Send event with type, id, retry
  - `send_comment()` - Keep-alive comment
  - `ping()` - Keep-alive ping
  - `close()` - Close connection
  - Statistics tracking

### TLS/SSL

- **Context Header**: `/Users/bengamble/FasterAPI/src/cpp/net/tls_context.h` (229 lines)
  - `TlsContextConfig` struct - Certs, keys, ALPN protocols, TLS versions
  - `TlsContext` class - OpenSSL SSL_CTX wrapper

- **Context Implementation**: `/Users/bengamble/FasterAPI/src/cpp/net/tls_context.cpp`
  - `create_server()` / `create_client()` - Factory methods
  - `load_cert_file()` / `load_key_file()` - PEM loading
  - `load_cert_mem()` / `load_key_mem()` - In-memory loading
  - ALPN configuration callbacks
  - OpenSSL initialization

- **Socket Header**: `/Users/bengamble/FasterAPI/src/cpp/net/tls_socket.h` (256 lines)
  - `TlsState` enum (HANDSHAKE_NEEDED, HANDSHAKE_IN_PROGRESS, CONNECTED, ERROR)
  - `TlsSocket` class - OpenSSL socket wrapper

- **Socket Implementation**: `/Users/bengamble/FasterAPI/src/cpp/net/tls_socket.cpp`
  - `accept()` / `connect()` - Factory methods
  - `handshake()` - Non-blocking TLS handshake
  - `read()` / `write()` / `flush()`
  - `process_incoming()` - Feed encrypted data to SSL
  - `get_alpn_protocol()` - Protocol negotiation result
  - Memory BIO architecture (rbio, wbio)

### HTTP/3 (Stub)

- **Header**: `/Users/bengamble/FasterAPI/src/cpp/http/http3_parser.h` (180 lines)
  - `HTTP3FrameType` enum - Frame types
  - `HTTP3FrameHeader` struct
  - `HTTP3Settings` struct
  - `HTTP3Parser` class - Skeleton parser
  - `QPACKDecoder` class - Stub decoder

- **Implementation**: `/Users/bengamble/FasterAPI/src/cpp/http/http3_parser.cpp`
  - Very minimal implementation (not production-ready)

## Key Statistics

| Component | Header Lines | Implementation Lines | Total |
|-----------|--------------|----------------------|-------|
| HTTP/1.x Parser | 247 | 300 | 547 |
| HTTP/1.x Connection | 241 | TBD | ~400 |
| HTTP/2 Frames | 350 | 546 | 896 |
| HTTP/2 HPACK | 327 | 586 | 913 |
| HTTP/2 Streams | 320 | TBD | ~600 |
| HTTP/2 Connection | 314 | 537 | 851 |
| HTTP/2 Server Push | 183 | TBD | ~300 |
| WebSocket Parser | 235 | 400 | 635 |
| WebSocket Connection | 211 | 350 | 561 |
| SSE | 220 | 230 | 450 |
| TLS Context | 229 | TBD | ~400 |
| TLS Socket | 256 | TBD | ~500 |
| HTTP/3 (Stub) | 180 | TBD | ~100 |
| **TOTAL** | | | **~10,674** |

## Build Files

- **CMakeLists.txt** - Project configuration
  - HTTP/1.x: http1_parser.cpp, http1_connection.cpp
  - HTTP/2: http2_frame.cpp, http2_stream.cpp, http2_connection.cpp, hpack.cpp, h2_server_push.cpp, http2_server.cpp
  - WebSocket: websocket_parser.cpp, websocket.cpp
  - SSE: sse.cpp
  - TLS: tls_context.cpp, tls_socket.cpp
  - HTTP/3: http3_parser.cpp
  - Huffman: huffman.cpp
  - Support: json_parser.cpp, compression.cpp, middleware.cpp, router.cpp, etc.

## Important Interfaces

### All protocols use same callback pattern
```cpp
using RequestCallback = std::function<Response(const Request&)>;
connection.set_request_callback(handler);
```

### All use similar I/O pattern
```cpp
// Processing input
result<size_t> process_input(const uint8_t* data, size_t len);

// Getting output
bool get_output(const uint8_t** out_data, size_t* out_len);
void commit_output(size_t len);
```

### State machines used
- HTTP/1: START → METHOD → URL → VERSION → HEADER → COMPLETE
- HTTP/2 Connection: IDLE → PREFACE_PENDING → ACTIVE → GOAWAY_* → CLOSED
- HTTP/2 Stream: IDLE → OPEN → HALF_CLOSED → CLOSED
- WebSocket Frame: READING_HEADER → READING_PAYLOAD_LENGTH → READING_PAYLOAD → COMPLETE
- TLS: HANDSHAKE_NEEDED → HANDSHAKE_IN_PROGRESS → CONNECTED

## Performance Characteristics

From code comments:
- HTTP/1.1 request line: <50ns
- HTTP/1.1 headers: <30ns each
- HPACK decode: <500ns per header
- HPACK encode: <300ns per header
- HTTP/2 frame build: <200ns
- Zero allocations during frame processing
- Preallocated buffers: 32KB input, ring output
- Buffer pools: 16x16KB frame buffers, 8x8KB header buffers

## Standards Compliance

- **HTTP/1.1**: RFC 7230-7235 (via http1_parser)
- **HTTP/2**: RFC 7540 (all frame types, HPACK, flow control)
- **HPACK**: RFC 7541 (compression, static/dynamic tables)
- **WebSocket**: RFC 6455 (framing, handshake, opcodes)
- **SSE**: WHATWG HTML spec (event streaming)
- **TLS**: OpenSSL (1.2, 1.3, ALPN, SNI)

## Testing & Examples

- **Examples**:
  - `/Users/bengamble/FasterAPI/examples/websocket_demo.py`
  - `/Users/bengamble/FasterAPI/examples/sse_demo.py`

- **Tests**:
  - `/Users/bengamble/FasterAPI/test_http1_simple` - HTTP/1.1
  - `/Users/bengamble/FasterAPI/test_http2_debug.py` - HTTP/2
  - `/Users/bengamble/FasterAPI/test_async_http2.py` - Async HTTP/2

- **Benchmarks**:
  - `/Users/bengamble/FasterAPI/benchmarks/bench_http2.*` - HTTP/2 performance

## Architecture Summary

### Layer 1: Protocols (src/cpp/http/ and src/cpp/net/)
- HTTP/1.x parser & connection
- HTTP/2 frames, streams, connection, HPACK, server push
- WebSocket parser & connection
- SSE connection manager
- TLS context & socket wrapper

### Layer 2: Network I/O (src/cpp/net/)
- Event loop (kqueue/epoll)
- TCP listener & socket
- TLS socket wrapper
- Non-blocking I/O

### Layer 3: Application (src/cpp/http/)
- HTTP/2 server with coroutine support
- Python callback bridge
- Request routing
- Middleware pipeline

### Layer 4: Python Integration
- ctypes FFI bindings
- Sub-interpreter executor
- Async/await support
- GIL management

## Next Steps for Development

1. **HTTP/2 Prioritization** - Structure exists, not implemented
2. **HTTP/2 Coalescing** - Stream data coalescing
3. **WebSocket Compression** - permessage-deflate (structure exists)
4. **HTTP/3** - Complete from stub (QUIC integration needed)
5. **Connection Pooling** - Reuse across requests
6. **Performance Tuning** - Vectorization, SIMD

## Key Files to Review

**For understanding HTTP/1.1**:
- `/Users/bengamble/FasterAPI/src/cpp/http/http1_parser.h`
- `/Users/bengamble/FasterAPI/src/cpp/http/http1_connection.h`

**For understanding HTTP/2**:
- `/Users/bengamble/FasterAPI/src/cpp/http/http2_frame.h` - Frame types
- `/Users/bengamble/FasterAPI/src/cpp/http/hpack.h` - Compression
- `/Users/bengamble/FasterAPI/src/cpp/http/http2_stream.h` - Streams
- `/Users/bengamble/FasterAPI/src/cpp/http/http2_connection.h` - Connection

**For understanding WebSocket**:
- `/Users/bengamble/FasterAPI/src/cpp/http/websocket_parser.h` - Frame parsing
- `/Users/bengamble/FasterAPI/src/cpp/http/websocket.h` - Connection

**For understanding TLS**:
- `/Users/bengamble/FasterAPI/src/cpp/net/tls_context.h` - Certificate setup
- `/Users/bengamble/FasterAPI/src/cpp/net/tls_socket.h` - Connection

