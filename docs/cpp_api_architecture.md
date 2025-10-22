# FasterAPI C++ API Architecture

## Overview

FasterAPI uses a **three-layer architecture** to provide high-performance functionality to Python while maintaining clean separation of concerns:

```
┌─────────────────────────────────────────┐
│         Python User API                 │  ← High-level Pythonic interface
│  (fasterapi/http/websocket.py, etc.)   │
└─────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────┐
│      Python ctypes Bindings             │  ← FFI layer (type conversion)
│    (fasterapi/http/bindings.py)        │
└─────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────┐
│         C Export Layer                  │  ← extern "C" functions
│    (src/cpp/http/*_lib.cpp)            │
└─────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────┐
│      C++ Implementation Layer           │  ← Internal implementation
│  (src/cpp/http/*.cpp, *.h)             │
└─────────────────────────────────────────┘
```

## Layer Responsibilities

### 1. C++ Implementation Layer

**Location**: `src/cpp/http/*.cpp`, `src/cpp/http/*.h`

**Purpose**: Internal C++ implementation with performance-critical code

**Examples**:
- `websocket.cpp` / `websocket.h` - WebSocket protocol implementation
- `sse.cpp` / `sse.h` - Server-Sent Events implementation
- `server.cpp` / `server.h` - HTTP server implementation

**Characteristics**:
- Modern C++20 features
- Template metaprogramming for performance
- RAII resource management
- Zero-copy operations where possible
- Lock-free data structures
- No Python dependencies

**Design Pattern**:
```cpp
namespace fasterapi {
namespace http {

class WebSocketConnection {
public:
    explicit WebSocketConnection(uint64_t connection_id, const Config& config);

    int send_text(const std::string& message);
    int send_binary(const uint8_t* data, size_t length);
    bool is_open() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  // PIMPL pattern
    // ...
};

} // namespace http
} // namespace fasterapi
```

### 2. C Export Layer

**Location**: `src/cpp/http/*_lib.cpp`

**Purpose**: Foreign Function Interface (FFI) boundary - exposes C API to Python

**Examples**:
- `websocket_lib.cpp` - C exports for WebSocket
- `sse_lib.cpp` - C exports for SSE
- `http_lib.cpp` - C exports for HTTP server

**Characteristics**:
- `extern "C"` linkage (no name mangling)
- Simple C-compatible types (void*, int, char*, etc.)
- No exceptions (compiled with `-fno-exceptions`)
- All functions marked `noexcept`
- Error codes via return values or output parameters
- Opaque pointers for object handles

**Design Pattern**:
```cpp
extern "C" {

/**
 * Create a new WebSocket connection.
 *
 * @param connection_id Unique connection ID (use 0 for auto-generation)
 * @return Connection handle (cast to WebSocketConnection*), or nullptr on error
 */
void* ws_create(uint64_t connection_id) noexcept {
    // Create C++ object
    auto conn = std::make_unique<fasterapi::http::WebSocketConnection>(connection_id);

    // Return opaque pointer
    return static_cast<void*>(conn.release());
}

/**
 * Send text message.
 *
 * @param ws Connection handle
 * @param message Null-terminated text message
 * @return 0 on success, error code otherwise
 */
int ws_send_text(void* ws, const char* message) noexcept {
    if (!ws || !message) return -1;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->send_text(std::string(message));
}

/**
 * Destroy a WebSocket connection.
 *
 * @param ws Connection handle from ws_create
 */
void ws_destroy(void* ws) noexcept {
    if (!ws) return;
    delete reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
}

}  // extern "C"
```

**Naming Convention**:
- Module prefix: `ws_` for WebSocket, `sse_` for SSE, `pg_` for PostgreSQL, etc.
- Object operations: `{module}_create`, `{module}_destroy`
- Methods: `{module}_{operation}` (e.g., `ws_send_text`, `sse_send`)
- Getters: `{module}_get_{property}` or `{module}_{property}` (e.g., `ws_is_open`)

### 3. Python ctypes Bindings

**Location**: `fasterapi/http/bindings.py`

**Purpose**: Load native library and define Python function signatures

**Characteristics**:
- Uses Python's `ctypes` library
- Defines argument and return types for all C functions
- Singleton pattern for library instance
- Automatic library discovery

**Design Pattern**:
```python
from ctypes import c_void_p, c_char_p, c_int, c_uint64, c_bool, CDLL

class _NativeLib:
    _instance = None
    _lib = None

    def _setup_functions(self):
        # WebSocketConnection* ws_create(uint64_t connection_id)
        self._lib.ws_create.argtypes = [c_uint64]
        self._lib.ws_create.restype = c_void_p

        # int ws_send_text(WebSocketConnection* ws, const char* message)
        self._lib.ws_send_text.argtypes = [c_void_p, c_char_p]
        self._lib.ws_send_text.restype = c_int

        # void ws_destroy(WebSocketConnection* ws)
        self._lib.ws_destroy.argtypes = [c_void_p]
        self._lib.ws_destroy.restype = None
```

### 4. Python User API

**Location**: `fasterapi/http/websocket.py`, `fasterapi/http/sse.py`, etc.

**Purpose**: High-level Pythonic interface

**Characteristics**:
- Pythonic naming (snake_case)
- Context managers
- Type hints
- Async/await support
- Error handling with exceptions
- Documentation

**Design Pattern**:
```python
from fasterapi.http.bindings import get_lib

class WebSocket:
    """High-level WebSocket connection."""

    def __init__(self, connection_id: int = 0):
        """Create a new WebSocket connection."""
        self._lib = get_lib()
        self._handle = self._lib.ws_create(connection_id)
        if not self._handle:
            raise RuntimeError("Failed to create WebSocket connection")

    def __del__(self):
        """Clean up resources."""
        if self._handle:
            self._lib.ws_destroy(self._handle)

    def send_text(self, message: str) -> None:
        """Send a text message."""
        result = self._lib.ws_send_text(self._handle, message.encode('utf-8'))
        if result != 0:
            raise RuntimeError(f"Failed to send message: {result}")

    @property
    def is_open(self) -> bool:
        """Check if connection is open."""
        return self._lib.ws_is_open(self._handle)
```

## Module Organization

### HTTP Module
```
src/cpp/http/
├── websocket.h           # C++ WebSocket class
├── websocket.cpp         # C++ WebSocket implementation
├── websocket_lib.cpp     # C exports for WebSocket
├── websocket_parser.h    # WebSocket frame parser
├── websocket_parser.cpp  # Parser implementation
├── sse.h                 # C++ SSE class
├── sse.cpp              # C++ SSE implementation
├── sse_lib.cpp          # C exports for SSE
├── server.h             # C++ HTTP server class
├── server.cpp           # C++ HTTP server implementation
└── http_lib.cpp         # C exports for HTTP server

fasterapi/http/
├── bindings.py          # ctypes bindings for all HTTP functions
├── websocket.py         # High-level WebSocket API
└── sse.py              # High-level SSE API
```

### PostgreSQL Module
```
src/cpp/pg/
├── pg_pool.h            # C++ connection pool class
├── pg_pool.cpp          # Pool implementation
├── pg_connection.h      # C++ connection class
├── pg_connection.cpp    # Connection implementation
└── pg_lib.cpp          # C exports for PostgreSQL

fasterapi/pg/
├── bindings.py         # ctypes bindings
└── pool.py            # High-level pool API
```

## Adding a New Module

To add a new module (e.g., "cache"), follow these steps:

### 1. Create C++ Implementation

**File**: `src/cpp/cache/cache.h`
```cpp
#pragma once
#include <string>
#include <cstdint>

namespace fasterapi {
namespace cache {

class Cache {
public:
    explicit Cache(size_t max_size);
    ~Cache();

    int set(const std::string& key, const std::string& value);
    int get(const std::string& key, std::string& value) const;
    bool exists(const std::string& key) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cache
} // namespace fasterapi
```

**File**: `src/cpp/cache/cache.cpp`
```cpp
#include "cache.h"
// Implementation details...
```

### 2. Create C Export Layer

**File**: `src/cpp/cache/cache_lib.cpp`
```cpp
#include "cache.h"
#include <cstring>

extern "C" {

void* cache_create(size_t max_size) noexcept {
    auto cache = std::make_unique<fasterapi::cache::Cache>(max_size);
    return cache.release();
}

void cache_destroy(void* cache) noexcept {
    if (!cache) return;
    delete reinterpret_cast<fasterapi::cache::Cache*>(cache);
}

int cache_set(void* cache, const char* key, const char* value) noexcept {
    if (!cache || !key || !value) return -1;
    auto* c = reinterpret_cast<fasterapi::cache::Cache*>(cache);
    return c->set(std::string(key), std::string(value));
}

int cache_get(void* cache, const char* key, char* out_buffer, size_t buffer_size) noexcept {
    if (!cache || !key || !out_buffer) return -1;
    auto* c = reinterpret_cast<fasterapi::cache::Cache*>(cache);
    std::string value;
    int result = c->get(std::string(key), value);
    if (result == 0 && value.size() + 1 <= buffer_size) {
        std::strcpy(out_buffer, value.c_str());
    }
    return result;
}

}  // extern "C"
```

### 3. Update CMakeLists.txt

Add your source files to the appropriate library target:

```cmake
if (FA_BUILD_CACHE)
    set(CACHE_SOURCES
        src/cpp/cache/cache.cpp
        src/cpp/cache/cache_lib.cpp
    )

    add_library(fasterapi_cache SHARED ${CACHE_SOURCES})
    # ... configure library
endif()
```

### 4. Create Python Bindings

**File**: `fasterapi/cache/bindings.py`
```python
from ctypes import c_void_p, c_char_p, c_int, c_size_t, CDLL

class _CacheLib:
    def __init__(self):
        self._lib = CDLL("libfasterapi_cache.so")
        self._setup_functions()

    def _setup_functions(self):
        # void* cache_create(size_t max_size)
        self._lib.cache_create.argtypes = [c_size_t]
        self._lib.cache_create.restype = c_void_p

        # void cache_destroy(void* cache)
        self._lib.cache_destroy.argtypes = [c_void_p]
        self._lib.cache_destroy.restype = None

        # int cache_set(void* cache, const char* key, const char* value)
        self._lib.cache_set.argtypes = [c_void_p, c_char_p, c_char_p]
        self._lib.cache_set.restype = c_int
```

### 5. Create High-Level Python API

**File**: `fasterapi/cache/__init__.py`
```python
from fasterapi.cache.bindings import get_lib

class Cache:
    def __init__(self, max_size: int = 1000):
        self._lib = get_lib()
        self._handle = self._lib.cache_create(max_size)

    def __del__(self):
        if self._handle:
            self._lib.cache_destroy(self._handle)

    def set(self, key: str, value: str) -> None:
        result = self._lib.cache_set(
            self._handle,
            key.encode('utf-8'),
            value.encode('utf-8')
        )
        if result != 0:
            raise RuntimeError("Cache set failed")
```

## Best Practices

### C++ Implementation Layer

1. **Use PIMPL pattern** for binary stability
2. **Mark functions `noexcept`** where appropriate
3. **Avoid exceptions** in release builds (`-fno-exceptions`)
4. **Use smart pointers** for automatic resource management
5. **Prefer lock-free structures** for concurrency
6. **Document performance characteristics** in comments

### C Export Layer

1. **All functions must be `extern "C"`**
2. **All functions must be `noexcept`**
3. **Use opaque pointers** (`void*`) for object handles
4. **Return error codes** (0 = success, negative = error)
5. **Validate all pointer arguments** before use
6. **Document all parameters** with Doxygen-style comments
7. **Use output parameters** for complex return values

### Python Bindings

1. **Define argtypes and restype** for all functions
2. **Use appropriate ctypes types** (c_void_p, c_char_p, etc.)
3. **Handle string encoding** (Python str → UTF-8 bytes)
4. **Singleton pattern** for library instance

### Python User API

1. **Use context managers** where appropriate
2. **Provide type hints** for all public APIs
3. **Raise exceptions** on errors (convert error codes)
4. **Add comprehensive docstrings**
5. **Follow PEP 8** naming conventions

## Performance Considerations

### Zero-Copy Operations

The C++ layer uses zero-copy techniques:
- String views (`std::string_view`) instead of copies
- Move semantics for large objects
- Memory-mapped I/O for file operations
- Direct buffer access for network I/O

### Memory Management

- C++ objects managed by smart pointers internally
- C export layer uses raw pointers (opaque handles)
- Connection registry in `*_lib.cpp` maintains object lifetime
- Python bindings handle cleanup via `__del__`

### Thread Safety

- C++ implementation uses lock-free structures where possible
- Connection registries use mutexes for thread safety
- Per-core event loops avoid lock contention
- Atomic operations for counters and flags

## Error Handling

### C++ Layer
```cpp
// Internal: exceptions OK (but avoided in release)
int WebSocketConnection::send_text(const std::string& message) {
    if (!open_) return -1;  // Error code
    // ...
    return 0;  // Success
}
```

### C Export Layer
```cpp
// FFI: no exceptions, error codes only
int ws_send_text(void* ws, const char* message) noexcept {
    if (!ws || !message) return -1;  // Invalid argument

    auto* conn = reinterpret_cast<WebSocketConnection*>(ws);
    return conn->send_text(message);
}
```

### Python Layer
```python
# High-level: convert error codes to exceptions
def send_text(self, message: str) -> None:
    result = self._lib.ws_send_text(self._handle, message.encode('utf-8'))
    if result != 0:
        raise RuntimeError(f"Failed to send text: error {result}")
```

## Testing Strategy

1. **C++ Unit Tests**: Google Test for C++ implementation
2. **C API Tests**: Call C functions from C test harness
3. **Integration Tests**: Python tests using ctypes bindings
4. **End-to-End Tests**: Full Python API tests

## Migration Guide

If you encounter old-style code that doesn't follow this architecture:

1. **Identify the layer** - is it C++, C export, or Python?
2. **Extract C++ implementation** into proper class files
3. **Create `*_lib.cpp`** with `extern "C"` exports
4. **Update bindings.py** with new function signatures
5. **Update high-level Python API** if needed
6. **Update CMakeLists.txt** to compile new files
7. **Add tests** for all layers

## References

- [ctypes documentation](https://docs.python.org/3/library/ctypes.html)
- [PostgreSQL module](../src/cpp/pg/) - reference implementation
- [WebSocket module](../src/cpp/http/websocket*.cpp) - modern example
- [CMakeLists.txt](../CMakeLists.txt) - build configuration
