# FasterAPI C++ HTTP Server Implementation Analysis

## Executive Summary

The FasterAPI C++ HTTP server is a **substantial, production-oriented** implementation (~17,900 lines of C++ code) with mixed levels of implementation maturity. The codebase demonstrates:

- **Real, working core architecture**: Multi-protocol HTTP server, routing, request/response handling
- **Significant incomplete features**: HTTP/3, JSON parsing, some middleware functions
- **High-performance optimizations**: Buffer pools, zero-copy parsing, lockfree queues
- **Strategic use of allocation pooling**: Following project guidelines on memory management

---

## 1. CORE SERVER ARCHITECTURE

### Server Entry Points

**Header**: `server.h`, **Implementation**: `server.cpp`

**Status**: REAL and FUNCTIONAL

- **HttpServer class** - Top-level server abstraction
  - Config struct with 13 configurable parameters
  - Router with compile-time route tree building
  - UnifiedServer delegation model for protocol handling
  - Move-only semantics (non-copyable)
  - Statistics tracking with atomics (thread-safe)

**Key Methods**:
- `add_route()` - Store routes in map + Router (dual storage for introspection)
- `start()` - Spawns UnifiedServer, registers bridge handler
- `stop()` - Graceful shutdown
- `is_running()` - Check atomic flag
- `handle_unified_request()` - Bridge handler connecting protocols to routes

**Memory Management**: Uses `std::unique_ptr` for Router and UnifiedServer

**Observations**:
- Routes are stored in TWO places: `routes_` (unordered_map) and Router (tree structure)
  - This is for dual purposes: introspection and fast matching
- Has TODO on line 159: "Consider making this non-blocking or running in thread pool"
  - Currently blocks on unified_server_->start()
- Protocol detection happens via `:protocol` pseudo-header
- Response data is extracted from HttpResponse and sent via UnifiedServer callback

---

## 2. C API BINDING LAYER

**Header**: `http_server_c_api.h`, **Implementation**: `http_server_c_api.cpp`

**Status**: PARTIALLY REAL (wrapper layer complete, but some bridging incomplete)

**Functions**:
- `http_lib_init()` - Initialize HTTP library
- `http_server_create()` - Create server with C handle (void*)
- `http_add_route()` - Register route by method/path/handler_id
- `http_server_start()` - Start server
- `http_server_stop()` - Stop server
- `http_register_python_handler()` - Register Python callable
- `http_get_route_handler()` - Retrieve handler from RouteRegistry

**Status**: The API surface is REAL, but the Python integration layer is partially incomplete.

---

## 3. ROUTING SYSTEM

### Router Implementation

**Header**: `router.h`, **Implementation**: `router.cpp`

**Status**: REAL and FUNCTIONAL

**Architecture**:
- **Trie-based route tree** with per-method storage
- **Three node types**: STATIC, PARAM (/{id}), WILDCARD (/*path)
- **Dual indexing**: Hash map (char → index) + indices string (backward compat)
- **Priority system**: Incremented per insertion for matching order

**Key Classes**:
1. **RouterNode**:
   - `path`: The segment value (e.g., "users" or "{id}")
   - `type`: NodeType enum (STATIC, PARAM, WILDCARD)
   - `children`: Vector of unique_ptr<RouterNode>
   - `child_map`: HashMap for O(1) first-char lookup
   - `handler`: Function pointer to route handler
   - `param_name`: Name of parameter if type=PARAM

2. **Router**:
   - `trees_`: Map of HTTP method → RouterNode tree root
   - `route_count_`: Atomic counter for statistics

3. **RouteParams**:
   - Vector of key-value pairs
   - Used during matching to collect path parameters

**Matching Algorithm**:
```cpp
match_route(node, path, params, pos):
  1. Static children (O(1) hash lookup first, then fallback to loop)
  2. Parameter children (extract until next / or end)
  3. Wildcard children (match rest of path)
```

**Insertion Algorithm**:
- Node splitting on common prefix (e.g., "users" and "user" → share "user")
- Debug logging via stderr (lines 202-222)
- Parameter name extraction from `/{name}` syntax

**Observations**:
- Has extensive debug output via cerr (not production-ideal, but useful)
- Backtracking in parameter matching is simplified (line 431: `params.clear()`)
- Route count is atomic but not used for load balancing
- Uses `std::make_unique` for allocation (exception-safe)
- **97 occurrences of `new`/`delete` in codebase** - mostly in Router via `std::make_unique`

---

## 4. HTTP/1.1 REQUEST/RESPONSE HANDLING

### Request Object

**Header**: `request.h`, **Implementation**: `request.cpp`

**Status**: REAL

- Zero-copy header access via string_view
- Method enum (GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, CONNECT, TRACE)
- Query parameter parsing
- Path parameter extraction
- Content-Length tracking
- Client IP handling with X-Forwarded-For support

**From Parsed Data**:
- Static factory method `from_parsed_data()` for UnifiedServer integration
- Converts method string to enum
- Stores headers, path, query, body

**TODOs**:
- Line 79: "Parse and validate JSON" - json_body() just returns raw body
- Line 100: "Get actual client IP from connection" - only reads headers

### Response Object

**Header**: `response.h`, **Implementation**: `response.cpp`

**Status**: REAL with minor incomplete features

**Features**:
- Fluent API (chaining methods)
- Status codes enum (200, 201, 204, 400, 401, 403, 404, 422, 429, 500, etc.)
- Response types: JSON, TEXT, HTML, BINARY, STREAM, FILE
- Header management with unordered_map
- Body content in string
- Compression support (zstd)
- Cookie handling
- Redirect support
- Content-Type management
- `to_http_wire_format()` for serialization

**Methods**:
- `status()`, `header()`, `content_type()` - Configuration
- `json()`, `text()`, `html()`, `binary()`, `file()` - Body types
- `stream()`, `write()`, `end()` - Streaming responses
- `compress()`, `compression_level()` - Compression control
- `cookie()`, `clear_cookie()` - Cookie management
- `get_body()`, `get_headers()`, `get_status_code()` - Getters for bridge

**TODO**:
- Streaming callback implementation details

### HTTP/1.1 Connection Handler

**Header**: `http1_connection.h`, **Implementation**: `http1_connection.cpp`

**Status**: REAL

**State Machine**:
```
READING_REQUEST → READING_BODY → PROCESSING → WRITING_RESPONSE
     ↓                                            ↓
  KEEPALIVE → (next request)              or CLOSING/ERROR
```

**Key Features**:
- Stateful connection lifecycle
- Keep-alive support
- Input/output buffering
- Parse request → invoke callback → format response
- Request callback type defined clearly
- Move semantics support

**Implementation Details**:
- Input buffer: `std::vector<uint8_t>` with incremental parsing
- Output buffer: Pre-built response with offset tracking
- Parser integration: Uses HTTP1Parser
- Keep-alive logic based on Connection header
- `reset_for_next_request()` for reuse

**Memory**: Uses vectors (dynamic allocation) but doesn't constantly new/delete

---

## 5. HTTP/1.1 PARSER

**Header**: `http1_parser.h`, **Implementation**: `http1_parser.cpp`

**Status**: REAL and FUNCTIONAL

**Zero-Allocation Design**:
- Uses `std::string_view` for all string data (no copies)
- Fixed array for headers: `std::array<Header, 100>`
- All references point back to original buffer
- No exceptions, returns int error codes

**Parser States**:
- START, METHOD, URL, VERSION, HEADER_FIELD, HEADER_VALUE, BODY, COMPLETE, ERROR

**Data Structures**:
```cpp
HTTP1Request {
  HTTP1Method method
  HTTP1Version version
  string_view method_str, url, path, query, fragment
  array<Header, 100> headers
  size_t header_count
  string_view body
  uint64_t content_length
  bool chunked, keep_alive, upgrade
}
```

**Parsing Strategy**:
- State machine based (from llhttp)
- Case-insensitive header matching
- Handles Content-Length and chunked encoding flags
- Detects keep-alive and Connection: upgrade

**Quality**: Production-grade with proper boundary checking

---

## 6. HTTP/2 SUPPORT

### HTTP/2 Connection Handler

**Header**: `http2_connection.h`, **Implementation**: `http2_connection.cpp`

**Status**: PARTIALLY REAL (substantial implementation, not all features complete)

**Real Components**:
1. **Connection State Machine**:
   ```cpp
   enum class ConnectionState {
     IDLE, PREFACE_PENDING, ACTIVE, GOAWAY_SENT, GOAWAY_RECEIVED, CLOSED
   }
   ```

2. **Settings Management**:
   - SETTINGS_HEADER_TABLE_SIZE
   - SETTINGS_ENABLE_PUSH
   - SETTINGS_MAX_CONCURRENT_STREAMS (100)
   - SETTINGS_INITIAL_WINDOW_SIZE
   - SETTINGS_MAX_FRAME_SIZE (16384 min, 16777215 max)
   - SETTINGS_MAX_HEADER_LIST_SIZE

3. **Buffer Pool** (Zero-allocation):
   ```cpp
   BufferPool<16384, 16> frame_buffer_pool_
   BufferPool<8192, 8> header_buffer_pool_
   ```
   - Pre-allocated pools of fixed-size buffers
   - Acquire/release semantics (RAII with PooledBuffer wrapper)
   - Exhaustion returns nullptr

4. **Stream Management**:
   - StreamManager for handling HTTP/2 streams
   - Stream IDs properly tracked
   - Request callback invocation per stream

5. **HPACK Integration**:
   - HPACKEncoder and HPACKDecoder instances
   - Header compression support

6. **Frame Processing** (REAL):
   - Preface validation (incremental, per RFC 7540 Section 3.5)
   - Frame header parsing
   - Input buffer assembly for partial frames
   - Frame type dispatch to handlers
   - Extensive debug logging via cerr

7. **Implemented Frame Handlers**:
   - `handle_settings_frame()` - Parse SETTINGS
   - `handle_headers_frame()` - Parse HEADERS
   - `handle_data_frame()` - Parse DATA
   - `handle_window_update_frame()` - Flow control
   - `handle_ping_frame()` - PING/PONG
   - `handle_rst_stream_frame()` - Reset stream
   - `handle_goaway_frame()` - Connection close

8. **Output System**:
   - `output_buffer_`: Vector<uint8_t> with offset tracking
   - `queue_frame()` - Add frame to output
   - `queue_data()` - Add raw bytes
   - `get_output()` / `commit_output()` - Send flow

9. **Response Sending**:
   - `send_response()` method encodes status, headers, body
   - Uses HPACK for header compression
   - Frames creation and queueing

**Incomplete/TODO**:
- HTTP/2 server push (h2_server_push.h/.cpp exists with stubs)
- Stream priorities
- Ping timeout handling
- Some error code handling

**Memory Pattern**:
- Uses buffer pools for frame buffers (avoiding malloc/free on hot path)
- Output buffer is std::vector (acceptable, one allocation)
- String_view usage minimizes copies

### HTTP/2 Frames

**Header**: `http2_frame.h`, **Implementation**: `http2_frame.cpp`

**Status**: REAL

**Frame Types**:
- DATA, HEADERS, PRIORITY, RST_STREAM, SETTINGS, PUSH_PROMISE, PING, GOAWAY, WINDOW_UPDATE, CONTINUATION

**FrameHeader Structure**:
```cpp
struct FrameHeader {
  uint32_t length
  uint8_t type
  uint8_t flags
  uint32_t stream_id
}
```

**Functions**:
- `parse_frame_header()` - Parse 9-byte header
- `Frame serialization methods for each type`

### HPACK (Header Compression)

**Header**: `hpack.h`, **Implementation**: `hpack.cpp`, `huffman.cpp`

**Status**: PARTIALLY REAL (static table complete, dynamic table and encoding real)

**HPACK Static Table**:
- 61 pre-defined header entries
- `HPACKStaticTable::get()` - Retrieve by index
- `HPACKStaticTable::find()` - Find index by name/value

**Dynamic Table**:
```cpp
class HPACKDynamicTable {
  int add(string_view name, string_view value)
  int get(size_t index, HPACKHeader& out)
  void set_max_size(size_t size)
}
```
- Circular buffer (ring) implementation
- Max 128 entries, 4096 bytes default
- Properly evicts old entries

**Huffman Coding**:
- 256-byte lookup table for encoding
- Huffman tree for decoding
- Pre-generated huffman_table_data.cpp (84KB of table data)

**Encoder/Decoder**:
```cpp
class HPACKEncoder {
  result<vector<uint8_t>> encode(const vector<HPACKHeader>&)
}

class HPACKDecoder {
  result<vector<HPACKHeader>> decode(const uint8_t*, size_t)
}
```

---

## 7. ASYNC/COROUTINE EVENT LOOP INTEGRATION

### Event Loop Pool

**Header**: `event_loop_pool.h`, **Implementation**: `event_loop_pool.cpp`

**Status**: REAL with platform-specific implementations

**Architecture**:
```cpp
enum platform {
  LINUX: SO_REUSEPORT - each worker accepts on same port
  NON_LINUX: Single acceptor distributes via lockfree queues
}
```

**Configuration**:
```cpp
struct Config {
  uint16_t port
  string host
  uint16_t num_workers (0 = auto detect)
  size_t queue_size
  HttpServer* server
  atomic<bool>* shutdown_flag
}
```

**Linux Implementation**:
- Multiple worker threads
- Each binds to same port with SO_REUSEPORT
- Kernel automatically distributes connections
- Scales linearly with CPU cores
- Called `run_worker_with_reuseport()`

**Non-Linux Implementation**:
- 1 acceptor thread + N worker threads
- Acceptor accepts connection, pushes to worker queue
- Lockfree queue: `WorkerQueue` with head/tail atomics
- Round-robin distribution via `next_worker_` atomic
- Queue structure:
  ```cpp
  struct WorkerQueue {
    vector<void*> items
    atomic<size_t> head, tail
    bool try_push(void*)
    void* try_pop()
  }
  ```

**Coroutine Integration**:
- Templates on socket type `TSocket`
- Uses `co_await socket.ReadSome()` and `WriteSome()`
- Coroutine defined as `handle_connection()` template function
- Returns `TVoidTask` (from CoroIO library)

**Connection Handling Coroutine** (lines 38-120+):
```cpp
template<typename TSocket>
static NNet::TVoidTask handle_connection(
    TSocket socket,
    HttpServer* server,
    atomic<bool>* shutdown_requested
)
```
- Reads from socket via coroutine
- Parses HTTP/1.1 request
- Invokes server routing
- Sends response
- Handles keep-alive loop
- Enforces 1MB request size limit
- Sends HTTP/1.1 400/413 on error

**Memory Patterns**:
- Accumulates data into string (with reserve(1024))
- Parses in-place without allocations (HTTP1Parser uses string_view)
- Coroutines handled by CoroIO runtime

---

## 8. PYTHON CALLBACK BRIDGE

**Header**: `python_callback_bridge.h`, **Implementation**: `python_callback_bridge.cpp`

**Status**: PARTIALLY REAL (registration and basic invocation real, sub-interpreter support incomplete)

**Key Components**:

1. **Handler Registration**:
   - `registration_queue_`: Lockfree SPSC queue (capacity: 1024)
   - `handlers_`: Static unordered_map<"METHOD:path" → (handler_id, PyObject*)>
   - Direct registration during init (bypasses queue to avoid overflow)
   - `register_handler()` - Called from Python/Cython
   - `poll_registrations()` - Drains queue (called by event loop)

2. **Handler Invocation**:
   - `invoke_handler()` - SYNCHRONOUS version (DEPRECATED comment says)
     - Takes method, path, headers, body
     - Returns HandlerResult struct
   - `invoke_handler_async()` - ASYNCHRONOUS version (returns Future)
     - Submits to SubinterpreterExecutor
     - Enable true parallelism via sub-interpreters

3. **Data Structures**:
   ```cpp
   struct HandlerResult {
     int status_code = 200
     string content_type = "text/plain"
     string body
     unordered_map<string, string> headers
   }

   struct SerializedRequest {
     string method, path
     unordered_map<string, string> headers
     string body
     int handler_id
     PyObject* callable
   }

   struct HandlerRegistration {
     string method, path
     int handler_id
     PyObject* callable
   }
   ```

4. **Type Conversion** (lines 73-100+):
   - Helper function `convert_to_python_type()` with SchemaType switch
   - Handles INTEGER, FLOAT, BOOLEAN, STRING conversions
   - Uses `std::strtol()`, `std::strtod()` (exception-free)
   - Returns Python objects: PyLong_FromLong, PyFloat_FromDouble, etc.

5. **RouteRegistry Integration**:
   - `set_route_registry()` / `get_route_registry()`
   - Stores pointer to RouteRegistry for metadata-aware extraction

**TODOs**:
- Line 165 (in python_callback_bridge.cpp): "Implement true sub-interpreter support with handler re-registration"
- Async invocation relies on SubinterpreterExecutor (not shown here, but referenced)

**Memory Management**:
- Py_INCREF/Py_DECREF for PyObject* reference counting
- Uses Python C API directly
- Lockfree queue uses SPSC (single producer single consumer)

---

## 9. ROUTE METADATA & PARAMETER EXTRACTION

### Route Metadata

**Header**: `route_metadata.h`, **Implementation**: `route_metadata.cpp`

**Status**: REAL

**Data Structures**:
```cpp
enum ParameterLocation {
  PATH, QUERY, BODY, HEADER, COOKIE
}

struct ParameterInfo {
  string name
  SchemaType type
  ParameterLocation location
  bool required
  string default_value
  string description
}

struct RouteMetadata {
  string method, path_pattern
  CompiledRoutePattern compiled_pattern
  vector<ParameterInfo> parameters
  string request_body_schema
  string response_schema
  PyObject* handler
  string summary, description
  vector<string> tags
  unordered_map<int, string> responses  // status → description
}
```

**RouteRegistry**:
```cpp
class RouteRegistry {
  int register_route(RouteMetadata)
  const RouteMetadata* match(string method, string path)
  const vector<RouteMetadata>& get_all_routes()
  void clear()

private:
  vector<RouteMetadata> routes_
  unordered_map<string, vector<size_t>> method_index_
}
```

**RouteMetadataBuilder**:
- Fluent API for building metadata
- Methods: `path_param()`, `query_param()`, `request_schema()`, `response_schema()`
- Methods: `handler()`, `summary()`, `description()`, `tag()`, `build()`

---

## 10. MEMORY MANAGEMENT PATTERNS

### Allocation Philosophy

**Project Guidelines Compliance**:
- ✅ "allocations are expensive, we want preallocated buffers and pools"
- ✅ "avoid new, delete, malloc, free if possible"
- ✅ "use object pools and ring buffers by preference"

**Real Patterns Found**:

1. **Buffer Pools** (HTTP/2 Connection):
   - `BufferPool<16384, 16>` for frame data (16 × 16KB = 256KB pre-allocated)
   - `BufferPool<8192, 8>` for headers (8 × 8KB = 64KB pre-allocated)
   - RAII wrapper: `PooledBuffer` with acquire/release
   - Zero allocations for parsing within pool capacity

2. **Lockfree Queues** (Python Callback Bridge):
   - `AeronSPSCQueue<HandlerRegistration>` - single producer, single consumer
   - Capacity: 1024 entries (pre-allocated at construction)
   - No locks, no mutexes
   - Try_pop() / try_push() non-blocking

3. **Ring Buffers** (HTTP/2):
   - Dynamic table uses circular buffer internally
   - HPACKDynamicTable with head/tail indices

4. **Smart Pointers**:
   - `std::unique_ptr` for ownership (Router, UnifiedServer, TcpListener, etc.)
   - `std::shared_ptr` for TLS context
   - **No raw `new`/`delete` in public API** (uses make_unique)
   - **But 97 occurrences total** - mostly in implementation details

5. **Vector Reuse**:
   - Input/output buffers use `std::vector` with reserve()
   - Vectors reused per connection lifecycle
   - Not constant malloc/free on hot path

**Allocation Counting** (found 97 in entire http directory):
- Router tree node creation: ~20 (make_unique)
- HTTP/2 handler stubs: ~8
- General allocation: Mostly in make_unique calls (safe)
- **Very few raw new/delete - good compliance**

---

## 11. REAL VS INCOMPLETE/FAKE CODE

### REAL & WORKING

✅ **Core Architecture**:
- HttpServer, Router, routing trie
- Request/Response objects
- HTTP/1.1 parser and connection handler
- Event loop integration with CoroIO
- Python callback bridge registration

✅ **HTTP/1.1 Support**:
- Complete parser with zero-copy string_view
- Connection state machine with keep-alive
- Full request/response lifecycle
- Multiple content types (JSON, HTML, text, binary)

✅ **HTTP/2 Support** (Substantial):
- Connection state machine
- Settings frame handling
- HPACK static/dynamic tables
- Frame parsing and serialization
- Stream management basics
- Preface validation
- Buffer pools for zero-allocation parsing
- Header compression

✅ **High-Performance Features**:
- Buffer pool architecture
- Lockfree queues for Python integration
- Zero-copy header parsing
- Connection multiplexing (HTTP/2)

### INCOMPLETE/TODO

❌ **HTTP/3**:
- `h3_handler.h/cpp` and `http3_parser.h/cpp` - Extensive TODOs (8+ TODO comments)
- QPACK header parsing stubbed (line 18 of http3_parser.cpp)
- Not actually used in unified_server

❌ **HTTP/2 Server Push**:
- `h2_server_push.h/cpp` exists but not integrated
- Header generation exists but push not triggered

❌ **JSON Parsing**:
- `json_parser.h/cpp` - 14 TODO comments
- "TODO: Implement proper JSON parsing with simdjson"
- Currently just returns raw body strings

❌ **Streaming Responses**:
- `stream()`, `write()`, `end()` methods defined but not fully implemented
- Callback exists but not tested

❌ **File Serving**:
- `file()` method stub exists
- TODO: "Implement file serving with proper MIME type detection"

❌ **Middleware**:
- Basic structure exists in app.cpp
- TODOs for:
  - Role checking logic
  - Rate limiting logic
  - Proper middleware chain merging

❌ **WebSocket/SSE**:
- Basic interfaces exist (websocket.h, sse.h)
- Not integrated with unified_server

---

## 12. HARDCODED VALUES & SHORTCUTS

### Hardcoded Values Found

1. **Buffer Sizes**:
   - HTTP/2 frame buffer pool: 16384 (16KB)
   - HTTP/2 header buffer pool: 8192 (8KB)
   - Pool counts: 16 and 8
   - HPACK dynamic table: 4096 bytes max, 128 entries max

2. **HTTP/2 Settings**:
   - Max concurrent streams: 100
   - Initial window size: 65535 bytes
   - Header table size: 4096 bytes
   - Max header list size: 8192 bytes

3. **Limits**:
   - Max HTTP headers: 100 (HTTP1Request)
   - Max request size: 1MB (event_loop_pool.cpp line 29)
   - Router max stream ID: uint32_t implicit

4. **Ports**:
   - HTTP/1.1: 8080
   - HTTPS/HTTP/2: 443 (via tls_port in config)
   - TLS port = HTTP/1.1 port + 1 (line 130 server.cpp)

### Shortcuts Found

1. **Parameter Backtracking** (router.cpp line 431):
   ```cpp
   while (params.size() > param_count_before) {
     params.clear();  // Simple clear for now
   }
   ```
   - Comment acknowledges this is simplified
   - Works but not optimal (clears all params, not just one)

2. **Debug Logging** (router.cpp, http2_connection.cpp):
   - Extensive `std::cerr` output in production code
   - Example: router.cpp line 202-222 has detailed insertion logging
   - Example: http2_connection.cpp lines 46-56 have frame validation logging
   - Not performance-ideal for production

3. **Client IP Extraction** (app.cpp line 100):
   - Reads X-Forwarded-For header
   - Has TODO: "Get actual client IP from connection"
   - Falls back to empty string if no headers

4. **JSON Parsing** (app.cpp line 79, json_parser.cpp):
   - json_body() just returns raw string body
   - Multiple TODOs in json_parser.cpp for simdjson integration
   - Parser is incomplete

---

## 13. ASYNC/COROUTINE DETAILS

### CoroIO Integration

**Type**: C++ coroutines (co_await syntax)

**Used In**:
- `handle_connection()` template in event_loop_pool.cpp
- Awaits socket I/O operations

**Syntax**:
```cpp
auto bytes_read = co_await socket.ReadSome(buffer, sizeof(buffer));
co_await socket.WriteSome(data, len);
```

**Pattern**:
- Event loop spawns worker threads
- Each thread runs CoroIO event loop
- Connections handled as coroutines
- No blocking threads, true async

**Limitations**:
- Python callback invocation is currently synchronous (blocking)
- Async version exists but relies on SubinterpreterExecutor (not shown)

---

## 14. UNIFIED SERVER IMPLEMENTATION

**Header**: `unified_server.h`, **Implementation**: `unified_server.cpp`

**Status**: PARTIALLY REAL (substantial but TLS connection handling incomplete)

**Components**:

1. **Server Setup** (lines 28-114):
   - TLS context creation with auto-generated certificates
   - ALPN protocol negotiation setup
   - Listener creation for TLS and cleartext

2. **Certificate Generation** (lines 60-85):
   - Calls `net::TlsCertGenerator::generate()`
   - Auto-generates self-signed certs if none provided
   - Supports file-based or in-memory certs

3. **Listeners**:
   - TLS listener on port 443 (configurable)
   - Cleartext listener on port 8080 (configurable)
   - Both created from `net::TcpListener`

4. **Threading**:
   - TLS listener runs in background thread if both enabled
   - Cleartext runs in main thread (blocks on listener.start())

5. **Connection Handlers**:
   - Static method: `on_tls_connection()`
   - Static method: `on_cleartext_connection()`
   - Static method: `handle_tls_connection()`
   - Static method: `handle_http2_connection()`
   - Static method: `handle_http1_connection()`

**TODO** (line 193):
- "Implement HTTP/2 connection handling with TLS I/O"
- Suggests HTTP/2 over TLS isn't fully wired

**Global State**:
- `s_request_handler_`: Static function<> for request dispatch
- `s_app_instance_`: Static App* for direct handling
- Thread-local sockets and connections

---

## 15. TESTING & VALIDATION OBSERVATIONS

### Good Signs
- Router has comprehensive debug output
- HTTP/2 frame parsing includes detailed validation
- HPACK includes static table correctness checks
- HTTP/1.1 parser is well-tested (production-quality)
- Use of string_view shows zero-copy awareness

### Testing Gaps
- HTTP/3 is completely untested
- JSON parsing untested (stubbed)
- WebSocket integration untested
- Middleware chains not tested in integration
- Sub-interpreter async execution untested

---

## 16. SUMMARY OF FINDINGS

### Code Quality Metrics

| Category | Status | Confidence |
|----------|--------|------------|
| HTTP/1.1 | PRODUCTION READY | 95% |
| HTTP/2 | SUBSTANTIAL, MOSTLY FUNCTIONAL | 70% |
| HTTP/3 | STUB/INCOMPLETE | 5% |
| Routing | PRODUCTION READY | 95% |
| Memory Management | FOLLOWS GUIDELINES | 90% |
| Python Integration | PARTIAL (registration good, async TBD) | 60% |
| Error Handling | GOOD (no exceptions, int codes) | 85% |

### Real Allocations
- **~17,876 lines** of substantial C++ code
- **97 malloc/new/free** occurrences - mostly in make_unique
- **Buffer pools** properly implemented for hot paths
- **Lockfree queues** used for Python/C++ boundary

### Incomplete Features
1. HTTP/3 (8+ TODOs, not integrated)
2. JSON parsing (14 TODOs, awaiting simdjson)
3. Server push (exists but not triggered)
4. Streaming responses (partial)
5. Middleware chains (basic structure only)

### Architecture Strengths
1. ✅ Multi-protocol design (HTTP/1.1, HTTP/2, HTTP/3 planned)
2. ✅ Zero-copy parsing (string_view throughout)
3. ✅ Buffer pools (HTTP/2 frames, HPACK dynamic table)
4. ✅ Lockfree synchronization (Python callback registration)
5. ✅ Coroutine-based async I/O (via CoroIO)
6. ✅ Type-safe routing (method-based trees)

### Architecture Weaknesses
1. ❌ Debug logging in production code (cerr)
2. ❌ Some error conditions just return error codes (no context)
3. ❌ Double storage of routes (map + tree) adds memory
4. ❌ Parameter matching backtracking is crude
5. ❌ TLS/HTTP/2 integration incomplete
