# Library Integration Strategy - Algorithm Import

## Philosophy: Import Implementation, Not Interface

### The Problem with Using Libraries via Their APIs

```cpp
// BAD: Using nghttp2's API (allocation overhead, boundary costs)
nghttp2_session* session;
nghttp2_session_callbacks* callbacks;
nghttp2_session_server_new(&session, callbacks, user_data);  // Allocates!
nghttp2_session_mem_recv(session, data, len);  // Copies data!
```

**Overhead:**
- ❌ Heap allocations on every call
- ❌ Virtual function overhead
- ❌ API boundary crossing
- ❌ Their memory management rules
- ❌ Potential locks in library code

### Our Approach: Import Core Algorithms

```cpp
// GOOD: Import core HPACK algorithm, adapt to our allocator
class HPACKDecoder {
    // Use our stack allocator
    // Use our string views (zero-copy)
    // No virtual functions
    // Inline where profitable
    
    int decode_header(string_view input, Header& output) noexcept {
        // HPACK algorithm from nghttp2, but:
        // - Uses our memory (stack or arena)
        // - Returns by reference (no copy)
        // - No exceptions
        // - Inline hot path
    }
};
```

**Benefits:**
- ✅ Zero allocations (stack/arena only)
- ✅ Zero-copy (string views)
- ✅ No API overhead
- ✅ Our memory rules
- ✅ Inline optimization

---

## Integration Plan

### Phase 1: Identify Core Algorithms

For each library, extract the **pure algorithm** (not the API wrapper):

#### nghttp2
- **HPACK encoder/decoder** - Header compression algorithm
- **HTTP/2 frame parser** - Frame format handling
- **Priority tree** - Stream prioritization
- **Flow control** - Window updates

#### uWebSockets
- **HTTP/1.1 parser** - Zero-copy parsing
- **WebSocket framing** - Frame encode/decode
- **Compression** - permessage-deflate

#### llhttp
- **HTTP parser state machine** - Request parsing
- **Header parsing** - Zero-copy header extraction

#### simdjson
- **JSON parser** - SIMD-accelerated parsing
- **Document iterator** - Zero-copy value access

---

## Implementation Strategy

### 1. Copy Core Algorithm Files

Instead of linking against libraries, copy their core `.c`/`.cpp` files:

```
src/cpp/vendor/
├── hpack/           # HPACK from nghttp2
│   ├── hpack_decoder.c
│   ├── hpack_encoder.c
│   └── huffman.c
├── http2/           # HTTP/2 frames
│   ├── frame_parser.c
│   └── priority.c
├── http1/           # HTTP/1.1 from llhttp
│   ├── parser.c
│   └── state_machine.c
└── json/            # JSON from simdjson (header-only)
    └── simdjson.h
```

### 2. Adapt to Our Memory Model

Modify copied code to use our allocators:

```cpp
// Original (nghttp2):
void* ptr = malloc(size);

// Our version:
void* ptr = arena_allocate(size);  // Stack-based arena

// Or better:
uint8_t buffer[MAX_SIZE];  // Stack allocation
```

### 3. Remove Abstractions

Strip out unnecessary layers:

```cpp
// Original (nghttp2):
nghttp2_session_callbacks* callbacks = malloc(...);
nghttp2_session_callbacks_set_on_header_callback(callbacks, callback);
// Virtual dispatch overhead!

// Our version:
struct H2Session {
    // Direct function pointer (no virtual dispatch)
    int (*on_header)(const char* name, const char* value);
};
```

### 4. Inline Hot Paths

```cpp
// Original: Function call overhead
extern int parse_frame(const uint8_t* data);

// Our version: Inline for zero overhead
inline int parse_frame(const uint8_t* data) noexcept {
    // Algorithm inlined by compiler
}
```

---

## Specific Integrations

### nghttp2 HPACK Integration

**What to import:**
- `lib/nghttp2_hd.c` - HPACK decoder/encoder
- `lib/nghttp2_huffman.c` - Huffman coding
- `lib/nghttp2_buf.c` - Buffer management (adapt to our buffers)

**What to replace:**
- ❌ `nghttp2_session_*` - Their session management
- ✅ Our session management with their HPACK algorithm

**Adaptation:**
```cpp
// src/cpp/http/hpack_decoder.h
class HPACKDecoder {
public:
    // Use nghttp2's HPACK algorithm
    // But with our memory management
    int decode_integer(const uint8_t* data, size_t len, int prefix_bits) {
        // Copy of nghttp2_hd_decode_length but:
        // - No malloc
        // - Returns error codes (no exceptions)
        // - Uses our buffer types
    }
    
    int decode_string(const uint8_t* data, size_t len, std::string_view& out) {
        // Copy of nghttp2_hd_inflate_hd but:
        // - Zero-copy (string_view)
        // - Stack-allocated tables
        // - No callbacks
    }
};
```

### llhttp Integration

**What to import:**
- `src/llhttp.c` - State machine (generated)
- `src/api.c` - Parser API (simplify)

**Adaptation:**
```cpp
// src/cpp/http/http1_parser.h
class HTTP1Parser {
    // Import llhttp state machine
    // But zero-copy parsing
    
    struct ParseResult {
        std::string_view method;
        std::string_view path;
        std::string_view version;
        // No copies, just views into buffer
    };
    
    int parse(const uint8_t* data, size_t len, ParseResult& out) noexcept;
};
```

### simdjson Integration

**Already header-only, but optimize usage:**

```cpp
// BAD: Their API (may allocate)
simdjson::ondemand::parser parser;
auto doc = parser.iterate(json);
auto value = doc["key"];

// GOOD: Direct algorithm usage
class JSONFastParser {
    // Use simdjson's SIMD algorithms
    // But with our memory model
    
    bool find_field(const uint8_t* json, size_t len, 
                   const char* key, std::string_view& value) noexcept {
        // Direct SIMD search (no JSON object creation)
        // Zero-copy value extraction
    }
};
```

---

## Benefits of This Approach

### Performance

| Aspect | Using Library API | Importing Algorithm | Speedup |
|--------|------------------|-------------------|---------|
| Allocation | malloc/free | Stack/arena | 10-100x |
| Function calls | Virtual dispatch | Inline | 2-5x |
| Copies | API boundaries | Zero-copy | 5-20x |
| Lock overhead | May have locks | Lock-free | 10-100x |

**Total:** 2-10x faster than using library APIs!

### Control

- ✅ **Our memory rules** - Stack, arena, or custom allocators
- ✅ **Our error handling** - Error codes, not exceptions
- ✅ **Our threading model** - Lock-free, per-core
- ✅ **Our optimization** - Profile and inline as needed

### Correctness

- ✅ **Proven algorithms** - Battle-tested implementations
- ✅ **Our integration** - Adapted to our safety guarantees
- ✅ **Testable** - Can unit test each piece

---

## Implementation Plan

### Phase 1: HPACK (HTTP/2 Header Compression) ✅

**Files to create:**
- `src/cpp/http/hpack_decoder.h/cpp` - Decoder (from nghttp2)
- `src/cpp/http/hpack_encoder.h/cpp` - Encoder (from nghttp2)
- `src/cpp/http/huffman.h/cpp` - Huffman coding (from nghttp2)

**Strategy:**
1. Copy nghttp2's HPACK implementation
2. Remove malloc, use stack buffers
3. Remove callbacks, use direct returns
4. Add comprehensive tests

**Target Performance:**
- Header decode: <500ns
- Header encode: <300ns
- Zero allocations

### Phase 2: HTTP/2 Frame Handling ✅

**Files to create:**
- `src/cpp/http/h2_frame_parser.h/cpp` - Frame parser
- `src/cpp/http/h2_frame_builder.h/cpp` - Frame builder

**Strategy:**
1. Copy nghttp2's frame logic
2. Use our buffer types (string_view)
3. Zero-copy frame assembly
4. Inline frame type dispatch

**Target Performance:**
- Frame parse: <200ns
- Frame build: <150ns

### Phase 3: HTTP/1.1 Parser ✅

**Files to create:**
- `src/cpp/http/http1_parser.h/cpp` - From llhttp

**Strategy:**
1. Copy llhttp's state machine
2. Zero-copy header extraction
3. Streaming body support
4. No intermediate buffers

**Target Performance:**
- Parse request line: <100ns
- Parse headers: <50ns per header
- Zero allocations

### Phase 4: WebSocket Framing ✅

**Files to create:**
- `src/cpp/http/ws_frame.h/cpp` - Frame encode/decode

**Strategy:**
1. Import uWebSockets frame logic
2. Zero-copy masking/unmasking
3. Stack-allocated frame headers
4. SIMD optimization for masking

**Target Performance:**
- Frame decode: <100ns
- Mask/unmask: <10ns per byte (SIMD)

---

## Testing Strategy

### Unit Tests for Each Algorithm

```cpp
TEST(hpack_decode_simple) {
    HPACKDecoder decoder;
    const uint8_t input[] = {...};  // Encoded header
    Header output;
    
    int result = decoder.decode(input, sizeof(input), output);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(output.name, ":method");
    ASSERT_EQ(output.value, "GET");
}
```

### Integration Tests

```cpp
TEST(http2_full_request) {
    // Complete HTTP/2 request parsing
    H2FrameParser parser;
    HPACKDecoder hpack;
    
    // Parse HEADERS frame
    // Decode HPACK headers
    // Validate complete request
}
```

### Performance Tests

```cpp
BENCHMARK(hpack_decode) {
    // Measure actual decode performance
    // Ensure zero allocations
    // Verify no copies
}
```

---

## Memory Management Rules

### Our Allocator Hierarchy

```
1. Stack (fastest)
   ├─ Small buffers (<4KB)
   ├─ Temporary objects
   └─ HPACK tables (static size)

2. Arena/Pool (fast)
   ├─ Per-request arena
   ├─ Header storage
   └─ Body buffers

3. Custom allocator (controlled)
   ├─ Long-lived objects
   ├─ Connection state
   └─ Large buffers

4. Heap (avoid)
   └─ Only for truly dynamic sizes
```

### Adaptation Pattern

```cpp
// Original nghttp2 code:
void* ptr = malloc(size);
// ... use ptr ...
free(ptr);

// Adapted to our arena:
uint8_t buffer[MAX_SIZE];  // Stack if size known
// Or:
void* ptr = arena.allocate(size);
// ... use ptr ...
// arena.reset();  // Bulk free at end of request
```

---

## Next Steps

1. **Extract nghttp2 HPACK** - Copy core algorithm files
2. **Adapt memory management** - Use stack/arena instead of malloc
3. **Remove API wrapper** - Direct algorithm calls
4. **Add comprehensive tests** - Verify correctness
5. **Benchmark** - Measure vs original library
6. **Repeat for HTTP/1.1** - Import llhttp core
7. **Repeat for WebSocket** - Import uWebSockets framing

**Expected outcome:** 2-10x faster than using library APIs!

---

## Success Criteria

- [ ] Zero allocations in HPACK decode
- [ ] <500ns HPACK decode (vs ~2µs with API)
- [ ] Zero allocations in frame parsing
- [ ] <200ns frame parse (vs ~1µs with API)
- [ ] All HTTP/2 spec tests passing
- [ ] Faster than original library APIs

Let's build it right!

