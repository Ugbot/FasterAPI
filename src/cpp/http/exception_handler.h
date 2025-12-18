#pragma once

#include <Python.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <mutex>
#include "../core/future.h"

namespace fasterapi {
namespace http {

/**
 * Information extracted from a Python exception.
 *
 * Populated by extract_exception_info() while GIL is held.
 * Used to dispatch to appropriate handler via O(1) type lookup.
 */
struct ExceptionInfo {
    // Exception type name (e.g., "HTTPException", "ValidationError")
    std::string type_name;

    // Full qualified type name (e.g., "starlette.exceptions.HTTPException")
    std::string qualified_name;

    // HTTP status code (for HTTPException, default 500)
    int status_code = 500;

    // Exception detail/message
    std::string detail;

    // Additional headers (for HTTPException)
    std::unordered_map<std::string, std::string> headers;

    // Exception type classification for fast dispatch
    enum class Type : uint8_t {
        HTTP_EXCEPTION,      // starlette.exceptions.HTTPException
        VALIDATION_ERROR,    // pydantic ValidationError
        REQUEST_VALIDATION,  // fastapi.exceptions.RequestValidationError
        PYTHON_EXCEPTION,    // Generic Python exception
        UNKNOWN
    };
    Type type = Type::UNKNOWN;

    // Raw Python exception (borrowed reference, only valid while GIL held)
    PyObject* exc_type = nullptr;
    PyObject* exc_value = nullptr;
    PyObject* exc_traceback = nullptr;
};

/**
 * Response data from an exception handler.
 *
 * Contains all data needed to send HTTP response.
 */
struct ExceptionResponse {
    int status_code = 500;
    std::string content_type = "application/json";
    std::string body;
    std::unordered_map<std::string, std::string> headers;

    // Whether response is valid
    bool valid = true;

    // Create default 500 error response
    static ExceptionResponse internal_error() {
        ExceptionResponse resp;
        resp.status_code = 500;
        resp.content_type = "application/json";
        resp.body = R"({"detail":"Internal Server Error"})";
        return resp;
    }

    // Create response for HTTPException
    static ExceptionResponse from_http_exception(int status, const std::string& detail,
                                                  const std::unordered_map<std::string, std::string>& hdrs = {}) {
        ExceptionResponse resp;
        resp.status_code = status;
        resp.content_type = "application/json";
        resp.headers = hdrs;

        // Build JSON body - escape detail string
        resp.body = "{\"detail\":";
        if (detail.empty()) {
            resp.body += "null";
        } else {
            resp.body += "\"";
            for (char c : detail) {
                switch (c) {
                    case '"': resp.body += "\\\""; break;
                    case '\\': resp.body += "\\\\"; break;
                    case '\n': resp.body += "\\n"; break;
                    case '\r': resp.body += "\\r"; break;
                    case '\t': resp.body += "\\t"; break;
                    default: resp.body += c;
                }
            }
            resp.body += "\"";
        }
        resp.body += "}";
        return resp;
    }
};

/**
 * Registry for custom exception handlers.
 *
 * Provides O(1) lookup by exception type name with fallback chain.
 * Supports both sync and async Python handlers via then() dispatch.
 *
 * Thread-safe: Uses mutex for registration, lock-free for lookup in hot path.
 */
class ExceptionHandlerRegistry {
public:
    /**
     * Get singleton instance.
     */
    static ExceptionHandlerRegistry& instance() noexcept {
        static ExceptionHandlerRegistry registry;
        return registry;
    }

    /**
     * Register a Python exception handler.
     *
     * @param exc_type Qualified exception type name (e.g., "myapp.CustomError")
     * @param handler Python callable (incref'd, registry owns reference)
     * @param is_async Whether handler is async (coroutine function)
     */
    void register_handler(const std::string& exc_type, PyObject* handler, bool is_async);

    /**
     * Register a Python exception handler by exception class.
     *
     * @param exc_class Python exception class (not incref'd)
     * @param handler Python callable (incref'd, registry owns reference)
     * @param is_async Whether handler is async
     */
    void register_handler_for_class(PyObject* exc_class, PyObject* handler, bool is_async);

    /**
     * Check if a handler is registered for exception type.
     *
     * @param exc_type Qualified exception type name
     * @return true if handler registered
     */
    bool has_handler(const std::string& exc_type) const noexcept;

    /**
     * Handle an exception and return response.
     *
     * Dispatches to registered handler or returns default response.
     * Uses then() chain for async handlers.
     *
     * @param info Exception info (must have been extracted while GIL held)
     * @return Future containing response (immediately ready for sync handlers)
     */
    core::future<ExceptionResponse> handle_exception(const ExceptionInfo& info) noexcept;

    /**
     * Handle exception synchronously (for simple cases).
     *
     * Calls Python handler if registered, returns default response otherwise.
     * GIL MUST be held when calling this.
     *
     * @param info Exception info
     * @return Response
     */
    ExceptionResponse handle_exception_sync(const ExceptionInfo& info) noexcept;

    /**
     * Extract exception info from current Python error.
     *
     * REQUIRES GIL. Calls PyErr_Fetch internally.
     * After calling, Python error is cleared.
     *
     * @return Populated ExceptionInfo
     */
    static ExceptionInfo extract_exception_info() noexcept;

    /**
     * Clear all registered handlers.
     * Thread-safe.
     */
    void clear();

    // Non-copyable
    ExceptionHandlerRegistry(const ExceptionHandlerRegistry&) = delete;
    ExceptionHandlerRegistry& operator=(const ExceptionHandlerRegistry&) = delete;

private:
    ExceptionHandlerRegistry();
    ~ExceptionHandlerRegistry();

    // Registered Python handler info
    struct PythonHandler {
        PyObject* handler = nullptr;  // Owned reference
        bool is_async = false;
    };

    // Handler map: exception type name -> handler
    std::unordered_map<std::string, PythonHandler> handlers_;

    // Class-based handlers: PyObject* (exception class) -> handler
    // Uses pointer as key (identity comparison)
    std::unordered_map<PyObject*, PythonHandler> class_handlers_;

    // Mutex for thread-safe registration
    mutable std::mutex mutex_;

    // Default handlers for built-in exception types
    static ExceptionResponse default_http_exception_handler(const ExceptionInfo& info);
    static ExceptionResponse default_validation_error_handler(const ExceptionInfo& info);
    static ExceptionResponse default_exception_handler(const ExceptionInfo& info);

    // Invoke Python handler (GIL must be held)
    ExceptionResponse invoke_python_handler(const PythonHandler& handler,
                                            const ExceptionInfo& info) noexcept;

    // Find handler for exception (tries qualified name, simple name, class, base classes)
    const PythonHandler* find_handler(const ExceptionInfo& info) const noexcept;
};

} // namespace http
} // namespace fasterapi
