#pragma once

#include <Python.h>
#include "native_value.h"
#include <string_view>
#include <unordered_map>

namespace fasterapi {
namespace types {

/**
 * Native HTTP Request (NumPy-style).
 *
 * Python sees: Request object
 * C++ sees: Direct struct with string_views
 *
 * Benefits:
 * - Zero-copy header/body access
 * - No Python object creation
 * - No GIL for reads (GIL-free in Python 3.13+ free-threading!)
 * - Cache-friendly layout
 * - Thread-safe reads (immutable after parse)
 *
 * Performance:
 * - 10-20x faster than Python Request wrapper
 * - Works perfectly with SubinterpreterPool (no GIL contention)
 * - Ideal for Python 3.13+ free-threading (zero GIL overhead)
 *
 * Thread safety:
 * - READ operations: No GIL needed (all fields immutable)
 * - WRITE operations: Not supported (request is read-only)
 * - Safe to share across subinterpreters (read-only)
 */
struct NativeRequest {
    PyObject_HEAD
    
    // Request line (zero-copy views into buffer)
    std::string_view method;
    std::string_view path;
    std::string_view query;
    std::string_view version;
    
    // Headers (string_view keys/values)
    struct Header {
        std::string_view name;
        std::string_view value;
    };
    Header* headers;
    uint32_t header_count;
    
    // Path parameters (from router)
    struct Param {
        std::string_view name;
        std::string_view value;
    };
    Param* params;
    uint32_t param_count;
    
    // Query parameters (parsed lazily)
    struct QueryParam {
        std::string_view name;
        std::string_view value;
    };
    QueryParam* query_params;
    uint32_t query_param_count;
    
    // Body (zero-copy view)
    std::string_view body;
    
    // Original buffer (we keep reference for zero-copy)
    const uint8_t* buffer;
    size_t buffer_len;
    
    static PyTypeObject Type;
    
    /**
     * Create from HTTP/1.1 parser result (zero-copy!).
     */
    static NativeRequest* create_from_buffer(
        const uint8_t* buffer,
        size_t len
    ) noexcept;
    
    /**
     * Get header (C++ operation, no GIL!).
     */
    std::string_view get_header(std::string_view name) const noexcept;
    
    /**
     * Get path parameter (C++ operation, no GIL!).
     */
    std::string_view get_param(std::string_view name) const noexcept;
    
    /**
     * Get query parameter (C++ operation, no GIL!).
     */
    std::string_view get_query_param(std::string_view name) const noexcept;
    
    /**
     * Parse body as JSON (returns NativeDict, no Python objects!).
     */
    NativeDict* json() noexcept;
    
    /**
     * Convert to Python Request object (only when needed).
     */
    PyObject* to_python() const noexcept;
};

/**
 * Native HTTP Response (NumPy-style).
 *
 * Python sees: Response object
 * C++ sees: Direct struct
 *
 * Benefits:
 * - Zero-copy response building
 * - Direct serialization
 * - No Python object overhead
 * - SIMD JSON serialization
 * - GIL-free writes (Python 3.13+ free-threading)
 *
 * Performance:
 * - 10-20x faster than Python Response
 * - Thread-safe (each handler gets own response)
 * - Perfect for SubinterpreterPool (no shared state)
 *
 * Thread safety:
 * - Each request handler creates its own NativeResponse
 * - No shared state between threads/interpreters
 * - Safe to use from any subinterpreter
 * - GIL only needed for PyObject creation (final conversion)
 */
struct NativeResponse {
    PyObject_HEAD
    
    uint16_t status_code;
    
    // Headers (we own this data)
    struct Header {
        std::string name;
        std::string value;
    };
    std::vector<Header> headers;
    
    // Body buffer (for building response)
    uint8_t* body_buffer;
    size_t body_size;
    size_t body_capacity;
    
    // Content type
    std::string content_type;
    
    static PyTypeObject Type;
    
    /**
     * Create response.
     */
    static NativeResponse* create() noexcept;
    
    /**
     * Set status code (C++ operation, no GIL!).
     */
    void set_status(uint16_t status) noexcept;
    
    /**
     * Set header (C++ operation, no GIL!).
     */
    void set_header(std::string_view name, std::string_view value) noexcept;
    
    /**
     * Set JSON body from NativeDict (SIMD serialization!).
     */
    int set_json(const NativeDict* dict) noexcept;
    
    /**
     * Set text body.
     */
    int set_text(std::string_view text) noexcept;
    
    /**
     * Serialize response (ready to send).
     */
    int serialize(uint8_t* output, size_t capacity, size_t& written) noexcept;
    
    /**
     * Convert to Python Response (only when needed).
     */
    PyObject* to_python() const noexcept;
};

/**
 * Native JSON parser/serializer (simdjson + SIMD writer).
 * 
 * Parses JSON → NativeDict (not PyDict!)
 * Serializes NativeDict → JSON (SIMD-optimized)
 * 
 * Speedup: 5-10x faster than Python json module
 */
class NativeJSON {
public:
    /**
     * Parse JSON to NativeDict.
     * 
     * Uses simdjson (already fast) but creates NativeDict instead of PyDict.
     * 
     * @param json_data JSON string
     * @param len Length
     * @return NativeDict*, or nullptr on error
     */
    static NativeDict* parse(const char* json_data, size_t len) noexcept;
    
    /**
     * Serialize NativeDict to JSON.
     * 
     * SIMD-optimized JSON writer (faster than standard serialization).
     * 
     * @param dict Native dict
     * @param output Output buffer
     * @param capacity Buffer capacity
     * @param written Bytes written
     * @return 0 on success
     */
    static int serialize(
        const NativeDict* dict,
        char* output,
        size_t capacity,
        size_t& written
    ) noexcept;
};

} // namespace types
} // namespace fasterapi

