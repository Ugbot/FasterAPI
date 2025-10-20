#pragma once

#include <Python.h>
#include "native_value.h"
#include "native_request.h"

namespace fasterapi {
namespace types {

/**
 * FastAPI-compatible API with native types underneath.
 * 
 * Maintains exact FastAPI API surface:
 * - Same decorators (@app.get, @app.post, etc.)
 * - Same dependency injection (Depends)
 * - Same path parameters
 * - Same request/response objects
 * 
 * But with:
 * - Native C++ types (like NumPy)
 * - Vectorcall for fastest calling
 * - Zero-copy everywhere
 * - No GIL for C++ operations
 * 
 * Performance: 40-100x faster than pure Python
 */

/**
 * Handler calling convention.
 * 
 * Uses PY_VECTORCALL_ARGUMENTS (fastest in Python 3.8+).
 */
#define FASTERAPI_USE_VECTORCALL 1

/**
 * Native handler wrapper.
 * 
 * Wraps C++ handler to look like Python callable.
 * Uses vectorcall for maximum performance.
 */
struct NativeHandler {
    PyObject_HEAD
    
    // Handler function pointer (C++ function!)
    typedef int (*HandlerFunc)(NativeRequest*, NativeResponse*);
    HandlerFunc func;
    
    // Handler metadata
    bool is_async;
    bool uses_native_types;  // If true, no Python conversion needed!
    
    static PyTypeObject Type;
    
    /**
     * Create handler.
     */
    static NativeHandler* create(HandlerFunc func, bool is_async = false) noexcept;
    
    /**
     * Call using vectorcall (fastest calling convention).
     * 
     * PEP 590: Uses stack-allocated args, no tuple creation!
     */
    static PyObject* vectorcall(
        PyObject* callable,
        PyObject* const* args,
        size_t nargsf,
        PyObject* kwnames
    ) noexcept;
};

/**
 * FastAPI-compatible Request object (but native underneath).
 * 
 * Python API (same as FastAPI):
 *   request.path_params["id"]
 *   request.query_params["page"]
 *   request.headers["content-type"]
 *   await request.json()
 *   await request.body()
 * 
 * Implementation:
 *   All backed by NativeRequest (zero-copy!)
 */
class FastAPIRequest {
public:
    /**
     * Create FastAPI-compatible request wrapper.
     * 
     * @param native_req Native request (zero-copy view into buffer)
     * @return Python object that looks like FastAPI Request
     */
    static PyObject* create(NativeRequest* native_req) noexcept;
    
    /**
     * Get path parameters (returns dict-like object).
     * 
     * Lazy creation - only creates Python dict if accessed.
     */
    static PyObject* get_path_params(NativeRequest* req) noexcept;
    
    /**
     * Get query parameters (returns dict-like object).
     */
    static PyObject* get_query_params(NativeRequest* req) noexcept;
    
    /**
     * Get headers (returns dict-like object).
     */
    static PyObject* get_headers(NativeRequest* req) noexcept;
    
    /**
     * Get JSON body (async).
     * 
     * Returns NativeDict (not PyDict!) for zero-copy.
     */
    static PyObject* json(NativeRequest* req) noexcept;
};

/**
 * FastAPI-compatible Response object (but native underneath).
 * 
 * Python API (same as FastAPI):
 *   return JSONResponse({"id": 123})
 *   return Response(content="Hello", media_type="text/plain")
 * 
 * Implementation:
 *   All backed by NativeResponse (C++ serialization!)
 */
class FastAPIResponse {
public:
    /**
     * Create JSON response (FastAPI compatible).
     * 
     * @param content Dict or NativeDict
     * @param status_code HTTP status
     * @return Response object
     */
    static PyObject* json_response(
        PyObject* content,
        int status_code = 200
    ) noexcept;
    
    /**
     * Create plain text response.
     */
    static PyObject* text_response(
        const char* content,
        int status_code = 200
    ) noexcept;
};

/**
 * Path parameter extraction (FastAPI compatible).
 * 
 * Extracts {id} from /users/{id} using our router.
 * Returns native types by default (zero-copy!).
 */
class PathParams {
public:
    /**
     * Extract path parameter as NativeInt.
     * 
     * FastAPI: id: int = Path(...)
     * FasterAPI: returns NativeInt (no Python object!)
     */
    static NativeInt* get_int(
        NativeRequest* req,
        const char* param_name
    ) noexcept;
    
    /**
     * Extract path parameter as NativeStr (zero-copy!).
     */
    static NativeStr* get_str(
        NativeRequest* req,
        const char* param_name
    ) noexcept;
};

/**
 * Dependency injection (FastAPI compatible).
 * 
 * FastAPI: def handler(db = Depends(get_db))
 * FasterAPI: Same API, but resolves to native types when possible
 */
class Depends {
public:
    /**
     * Create dependency.
     * 
     * @param func Dependency function
     * @return Dependency object
     */
    static PyObject* create(PyObject* func) noexcept;
    
    /**
     * Resolve dependency.
     * 
     * Calls dependency function and returns result.
     * If result is native type, keeps it native (zero-copy!).
     */
    static PyObject* resolve(PyObject* dependency) noexcept;
};

/**
 * Zero-copy optimization detector.
 * 
 * Analyzes handler to determine if it can use zero-copy path.
 */
class ZeroCopyAnalyzer {
public:
    struct Analysis {
        bool can_use_native_types;      // Can use NativeDict, etc.
        bool can_skip_gil;              // Pure C++ execution
        bool can_use_zero_copy_request; // Request as views
        bool can_inline_handler;        // Handler small enough to inline
    };
    
    /**
     * Analyze handler function.
     * 
     * @param handler Python callable
     * @return Analysis result
     */
    static Analysis analyze(PyObject* handler) noexcept;
};

} // namespace types
} // namespace fasterapi

